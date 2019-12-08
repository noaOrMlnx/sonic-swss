
#include <sstream>
#include <inttypes.h>
#include <string>

#include "converter.h"
#include "monitorTXorch.h"
#include "timer.h"
#include "port.h"
#include "select.h"
#include "portsorch.h"
#include "orch.h"
#include "sai_serialize.h"
#include <array>

using namespace std;
using namespace swss;

#define STATE_DB_TX_STATE "port_state"
#define VALUE "value"
#define STATES_NUMBER 3

extern PortsOrch* gPortsOrch;

static const array<string, STATES_NUMBER> stateNames = {"OK", "NOT_OK", "UNKNOWN"};
static const string counterName = "SAI_PORT_STAT_IF_OUT_ERRORS";
static const string portNameMap = "COUNTERS_PORT_NAME_MAP";

MonitorTXOrch::MonitorTXOrch(TableConnector configDBTConnector, TableConnector stateDBTConnector):
    // listens to configdb changes
    Orch(configDBTConnector.first, configDBTConnector.second),
    m_countersDB(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_countersTable(new Table(m_countersDB.get(), "COUNTERS")),
    m_countersPortNameMap(new Table(m_countersDB.get(), COUNTERS_PORT_NAME_MAP)),
    m_stateTxErrTable(stateDBTConnector.first, stateDBTConnector.second),
    m_txErrConfigTable(configDBTConnector.first, configDBTConnector.second)

{
    initTimer();
    setDefaultConfigParam();
}

MonitorTXOrch::~MonitorTXOrch(void) 
{
    SWSS_LOG_ENTER();
}

void MonitorTXOrch::setDefaultConfigParam()
{
    vector<FieldValueTuple> fv;
    fv.emplace_back(VALUE, to_string(m_poolingPeriod));
    m_txErrConfigTable.set(POOLING_PERIOD_KEY, fv);
    fv.clear();
    fv.emplace_back(VALUE, to_string(m_threshold));
    m_txErrConfigTable.set(THRESHOLD_KEY, fv);
}

bool MonitorTXOrch::createPortNameMap() 
{
    SWSS_LOG_ENTER();
    map<string, Port> &portsList =  gPortsOrch->getAllPorts();
    for (auto &entry : portsList) 
    {
        string name = entry.first;
        SWSS_LOG_NOTICE("PORT NAME IS %s", name.c_str());
        Port p = entry.second;
        if (p.m_type != Port::PHY)
        {
            continue;
        }
        string oidStr;
        if (!m_countersPortNameMap->hget("", name, oidStr)) 
        {
            SWSS_LOG_ERROR("error getting port name from counters");
            return false;
        }
        m_portsStringsMap.emplace(p.m_alias, oidStr);
    }
    return true;
}


/*
initialize timer with default pooling period
*/
void MonitorTXOrch::initTimer() 
{
    SWSS_LOG_ENTER();
    auto interv = timespec {.tv_sec = m_poolingPeriod, .tv_nsec = 0};
    m_timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(m_timer, this, "TX_ERROR_POOLING_PERIOD");
    Orch::addExecutor(executor);
    SWSS_LOG_NOTICE("Initializing timer with %d seconds ...", m_poolingPeriod);
    m_timer->start();  
}

void MonitorTXOrch::setPoolingPeriod(const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    for (auto element : data) 
    {
        const auto &field = fvField(element);
        const auto &value = fvValue(element);
        if (field == VALUE) 
        {
            try 
            {
                // change the interval and reset timer
                m_poolingPeriod = stoi(value);
                auto interv = timespec { .tv_sec = m_poolingPeriod, .tv_nsec = 0 };
                m_timer->setInterval(interv);
                m_timer->reset();
                SWSS_LOG_NOTICE("Changing pooling_period value to %s", value.c_str());
            } 
            catch (...)
            {
                SWSS_LOG_WARN("Cannot change pooling_period to the value entered.");
            }
        }
        else 
        {
            SWSS_LOG_WARN("Unknown field value");
        }
    }

}

void MonitorTXOrch::setThreshold(const vector<FieldValueTuple>& data)
{
    SWSS_LOG_ENTER();
    for (auto element : data) 
    {
        const auto &field = fvField(element);
        const auto &value = fvValue(element);
        if (field == VALUE)
        {
            try
            {
                m_threshold = stoi(value);
                SWSS_LOG_NOTICE("Changing threshold value to %s", value.c_str());
            }
            catch(...) 
            {
                SWSS_LOG_WARN("Cannot change threshold to the value entered.");
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown field value");
        }
    } 
}

bool MonitorTXOrch::handleSetCommand(const string& key, const vector<FieldValueTuple>& data) 
{
    SWSS_LOG_ENTER();
    if (key == POOLING_PERIOD_KEY) 
    {
        setPoolingPeriod(data);

    } 
    else if (key == THRESHOLD_KEY) 
    {
        setThreshold(data);
    } 
    else 
    {
        SWSS_LOG_ERROR("Unknown command");
        return false;
    }
    return true;
}

void MonitorTXOrch::doTask(Consumer &consumer) 
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();
    if (table_name != CFG_TX_ERROR_TABLE_NAME) 
    {
        SWSS_LOG_ERROR("Invalid table name - %s", table_name.c_str());
        return;
    }

    auto it = consumer.m_toSync.begin();
    while(it != consumer.m_toSync.end()) 
    {
        KeyOpFieldsValuesTuple t = it->second;
        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fieldValues = kfvFieldsValues(t);
        
        if (op == SET_COMMAND) 
        {
            bool isSet = handleSetCommand(key, fieldValues);
            if (!isSet) 
            {
                SWSS_LOG_ERROR("Set command has not succeed.");
            }
        } 
        else 
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }
        it = consumer.m_toSync.erase(it);
    }
}

void MonitorTXOrch::insertStateToDb(string portAlias, PortState state)
{
    vector<FieldValueTuple> fieldValuesVector;
    fieldValuesVector.emplace_back(STATE_DB_TX_STATE, stateNames[state]);
    m_stateTxErrTable.set(portAlias, fieldValuesVector);
}

bool MonitorTXOrch::getTXStatistics() 
{
    for (auto &entry : m_portsStringsMap) 
    {
        string portAlias = entry.first;
        if (!getTXCountersByAlias(portAlias))
        {
            return false;
        }
    }
    return true;
}

/*
 * get tx counters from db by port id 
 * 
 */
bool MonitorTXOrch::getTXCountersByAlias(string portAlias) 
{
    SWSS_LOG_ENTER();

    string strValue;
    string oidStr = m_portsStringsMap.find(portAlias)->second;

    if (!m_countersTable->hget(oidStr, counterName, strValue)) 
    {
        SWSS_LOG_WARN("Error reading counters table");
        insertStateToDb(portAlias, UNKNOWN);
        SWSS_LOG_ERROR("Cannot take information from table for port %s", oidStr.c_str());
        return false;
    }
    m_txErrStatByPort.emplace(portAlias, stoul(strValue));
    return true; 
}

void MonitorTXOrch::updatePortsState() 
{
    SWSS_LOG_ENTER();

    for (auto &entry : m_txErrStatByPort) 
    {
        PortState currState = OK;
        // sai_object_id_t currPortId = entry.first;
        string portAlias = entry.first;
        string oidStr = m_portsStringsMap.find(portAlias)->second;
        uint64_t totalTxErr = entry.second;
        uint64_t prev = 0;
        if (m_prevTxCounters.count(portAlias) > 0) 
        {
            prev = m_prevTxCounters.find(portAlias)->second;
        }
        m_prevTxCounters.emplace(portAlias, totalTxErr);
        m_currTxCounters.emplace(portAlias, (totalTxErr - prev));

        if (m_currTxCounters.find(portAlias)->second > m_threshold)
        {
            currState = NOT_OK;
        }
        m_portsState.emplace(portAlias, currState);
        insertStateToDb(portAlias, currState);
    }
}

void MonitorTXOrch::doTask(SelectableTimer &timer) 
{
    // every time timer expires:
    SWSS_LOG_ENTER();

    // wait until all ports are ready
    if (!gPortsOrch->allPortsReady()) 
    {
        SWSS_LOG_WARN("Ports are not ready yet");
        return;
    }

    if (!isPortMapInitialized) 
    {
        //initiate the map of ports and names
        if (!createPortNameMap())
        {
            SWSS_LOG_WARN("Ports-names map is not ready yet ... ");
            return;
        }
        isPortMapInitialized = true;
    }
    if (getTXStatistics())
    {
        updatePortsState();
    }
    else 
    {
        SWSS_LOG_WARN("Counters are unavailable");
    }
}

