
#include <sstream>
#include <inttypes.h>

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

// to use when adding cli
// #define STATE_DB_PORT     "port_id"
#define STATE_DB_TX_STATE "port_state"

#define STATES_NUMBER 3

extern PortsOrch* gPortsOrch;
// extern sai_port_api_t * sai_port_api;

static const array<string, STATES_NUMBER> stateNames = {"OK", "NOT_OK", "UNKNOWN"};
static const string counterName = "SAI_PORT_STAT_IF_OUT_ERRORS";
static const string portNameMap = "COUNTERS_PORT_NAME_MAP";


// constructor
MonitorTXOrch::MonitorTXOrch(TableConnector configDBTConnector, TableConnector stateDBTConnector):
    // listens to configdb changes
    Orch(configDBTConnector.first, configDBTConnector.second),

    m_countersDB(new DBConnector(COUNTERS_DB, DBConnector::DEFAULT_UNIXSOCKET, 0)),
    m_countersTable(new Table(m_countersDB.get(), "COUNTERS")),
    m_countersPortNameMap(new Table(m_countersDB.get(), COUNTERS_PORT_NAME_MAP)),
    // connector to state db
    m_stateTxErrTable(stateDBTConnector.first, stateDBTConnector.second)    
    // m_cfgTxErrTable(configDBTConnector.first, configDBTConnector.second)

{
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("creating monitor tx orch ...");
    initTimer();
    SWSS_LOG_NOTICE("after init timer");
    isPortMapInitialized = false;
 
}

bool MonitorTXOrch::createPortNameMap() {
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("all ports are ready");
    map<string, Port> &portsList =  gPortsOrch->getAllPorts();
    for (auto &entry : portsList) {
        string name = entry.first;
        SWSS_LOG_NOTICE("PORT NAME IS %s", name.c_str());
        Port p = entry.second;
        if (p.m_type == Port::PHY) {
            string oidStr;
            SWSS_LOG_NOTICE("m alias is %s", p.m_alias.c_str());

            // hget
            if (!m_countersPortNameMap->hget("", name, oidStr)) {
                
                SWSS_LOG_ERROR("error getting port name from counters");
                return false;
            }
            SWSS_LOG_NOTICE("port oid is : %s", oidStr.c_str());
            // oidStr  should contain "oid:0x100011" for example
            m_portsStringsMap.emplace(p.m_port_id, oidStr);
        }
    }
    SWSS_LOG_NOTICE("exiting the createportname func");
    return true;
}


/*
initialize timer with default pooling period
*/
void MonitorTXOrch::initTimer() {
    SWSS_LOG_ENTER();
    auto interv = timespec {.tv_sec = m_poolingPeriod, .tv_nsec = 0};
    m_timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(m_timer, this, "TX_ERROR_POOLING_PERIOD");
    Orch::addExecutor(executor);
    SWSS_LOG_NOTICE("Initializing timer with %d seconds ...", m_poolingPeriod);
    m_timer->start();
    SWSS_LOG_NOTICE("timer has started");
  
}

bool MonitorTXOrch::handleSetCommand(const string& key, const vector<FieldValueTuple>& data) 
{
    SWSS_LOG_ENTER();
    // change pooling period
    if (key == POOLING_PERIOD_KEY) {
        for (auto element : data) {
            const auto &field = fvField(element);
            const auto &value = fvValue(element);

            // change the interval and reset timer

            if (field == "value") {
                SWSS_LOG_NOTICE("changing pooling period to %s", value.c_str());
                // m_poolingPeriod = chrono::seconds(to_uint<uint32_t>(value));
                m_poolingPeriod = stoi(value);
                auto interv = timespec { .tv_sec = m_poolingPeriod, .tv_nsec = 0 };
                m_timer->setInterval(interv);
                m_timer->reset();
            }
        }

    // change threshold val
    } else if (key == THRESHOLD_KEY) {

        for (auto element : data) {
            const auto &field = fvField(element);
            const auto &value = fvValue(element);

            if (field == "value") {
                SWSS_LOG_NOTICE("changing threshold to %s", value.c_str());
                m_threshold = stoi(value);
            }

        }
    
    } else {
        SWSS_LOG_ERROR("unknown command");
        return false;
    }
    return true;

}

void MonitorTXOrch::doTask(Consumer &consumer) {
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("inside dotask - consumer");

    /* check if needed */
    string table_name = consumer.getTableName();
    if (table_name != CFG_TX_ERROR_TABLE_NAME) {
        SWSS_LOG_ERROR("Invalid table name - %s", table_name.c_str());
    }

    auto it = consumer.m_toSync.begin();
    while(it != consumer.m_toSync.end()) {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);
        vector<FieldValueTuple> fieldValues = kfvFieldsValues(t);
        
        if (op == SET_COMMAND) 
        {
            bool isSet = handleSetCommand(key, fieldValues);
            if (!isSet) {
                SWSS_LOG_ERROR("set has not succeed");
            }
        } 
        else 
        {
            SWSS_LOG_ERROR("Unknown operation type %s", op.c_str());
        }

        consumer.m_toSync.erase(it++);

    }
}

// void MonitorTXOrch::doTask(Consumer &consumer) {
//     SWSS_LOG_ENTER();
//     SWSS_LOG_NOTICE("implementation for now ....");
// }



bool MonitorTXOrch::getTXStatistics() {
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("inside get statistics");

    for (auto &entry : m_portsStringsMap) {
        SWSS_LOG_NOTICE("inside for loop - port  ... ");

        sai_object_id_t currPortID = entry.first;
        if (!getTXCountersById(currPortID)){
            return false;
        }
    }
    return true;

}

/*
 * get tx counters from db by port id 
 * 
 */
bool MonitorTXOrch::getTXCountersById(sai_object_id_t portID) {
    SWSS_LOG_ENTER();

    string strValue;
    string oidStr = m_portsStringsMap.find(portID)->second;

    SWSS_LOG_NOTICE("getting tx-err counters to specific port %s", oidStr.c_str());

    if (!m_countersTable->hget(oidStr, counterName, strValue)) {
        SWSS_LOG_NOTICE("Error reading counters table");
        // insert unknown state to DB

        vector<FieldValueTuple> fieldValuesVector;
        fieldValuesVector.emplace_back(STATE_DB_TX_STATE, "UNKNOWN");
        m_stateTxErrTable.set(oidStr, fieldValuesVector);
        SWSS_LOG_ERROR("Cannot take information from table for port %s", oidStr.c_str());
        return false;
    }
    m_TX_ERR_stat_by_port.emplace(portID, stoul(strValue));
    SWSS_LOG_NOTICE("emplaced port %s in map of stat", oidStr.c_str());
    return true; 
}

// void MonitorTXOrch::insertStateToDB(sai_object_id_t portID, PortState state) {
//     vector<FieldValueTuple> fieldValuesVector;
//     fieldValuesVector.emplace_back(STATE_DB_TX_STATE, state);
//     if (!m_stateTxErrTable.set(sai_serialize_object_id(currPortId), fieldValuesVector)) {
//         SWSS_LOG_ERROR("set to state db has not succeed");
//     }
// }

void MonitorTXOrch::updatePortsState() {
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("inside update port state");
    
    // TODO: check if need to flush here  - clear table before inserting new states. 
    m_stateTxErrTable.flush();

    for (auto &entry : m_TX_ERR_stat_by_port) {
        PortState currState = OK;
        sai_object_id_t currPortId = entry.first;
        string oidStr = m_portsStringsMap.find(currPortId)->second;
        uint64_t totalTxErr = entry.second;
        uint64_t prev = 0;
        if (m_prevTXCounters.count(currPortId) > 0) {
            prev = m_prevTXCounters.find(currPortId)->second;
        }
        m_prevTXCounters.emplace(currPortId, totalTxErr);
        m_currTXCounters.emplace(currPortId, (totalTxErr - prev));

        if (m_currTXCounters.find(currPortId)->second > m_threshold){

            currState = NOT_OK;
        }
        // SWSS_LOG_NOTICE("port %s state is %s", sai_serialize_object_id(currPortId), stateNames[currState]);
        SWSS_LOG_NOTICE("port s state is s");

        m_portsState.emplace(currPortId, currState);

        vector<FieldValueTuple> fieldValuesVector;
        fieldValuesVector.emplace_back(STATE_DB_TX_STATE, stateNames[currState]);
  
        m_stateTxErrTable.set(oidStr, fieldValuesVector);
        SWSS_LOG_NOTICE("Set status s to port s");
    
    }
}


// every time timer expired:
void MonitorTXOrch::doTask(SelectableTimer &timer) {
    SWSS_LOG_ENTER();
    SWSS_LOG_NOTICE("inside do task- selectable timer");
    SWSS_LOG_NOTICE("timer expired");

    // wait until all ports are ready
    if (!gPortsOrch->allPortsReady()) {
        SWSS_LOG_WARN("ports are not ready yet");
        return;
    }

    if (!isPortMapInitialized) {
        //initiate the map of ports and names
        if (!createPortNameMap()){
            SWSS_LOG_ERROR("port name map is not ready yet ... ");
            return;
        }
        isPortMapInitialized = true;
    }

    if (getTXStatistics()){
        updatePortsState();
        SWSS_LOG_NOTICE("updated ports stated in state db");
    } else {
        SWSS_LOG_WARN("the counters are unavailable");
    }
}



