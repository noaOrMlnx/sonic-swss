#ifndef MONITORTX_ORCH_H
#define MONITORTX_ORCH_H

#include <map>
#include <string>

#include "orch.h"
#include "portsorch.h"
#include "table.h"
#include "selectabletimer.h"
#include "select.h"
#include "timer.h"

#define TX_POOLING_PERIOD "pooling_period"
#define TX_THRESHOLD "threshold"

#define POOLING_PERIOD_KEY  "POOLING_PERIOD"
#define THRESHOLD_KEY "THRESHOLD"

using namespace std;
using namespace swss;

// default pooling period & threshold
#define TX_DEFAULT_POOLING_PERIOD 10
#define TX_DEFAULT_THRESHOLD 100

extern "C" 
{
    #include "sai.h"
}
enum PortState {OK, NOT_OK, UNKNOWN};

class MonitorTXOrch : public Orch
{
    
    public:
        MonitorTXOrch(TableConnector configDBTConnector, TableConnector stateDBTConnector);
        virtual ~MonitorTXOrch(void);
        virtual void doTask(SelectableTimer &timer);
        virtual void doTask(Consumer &consumer);

    private:
        SelectableTimer *m_timer = nullptr;
        // counters table - taken from redis 
        shared_ptr<swss::DBConnector> m_countersDB = nullptr;
        shared_ptr<swss::Table> m_countersTable = nullptr;
        shared_ptr<Table> m_countersPortNameMap = nullptr;
        // state table
        Table m_stateTxErrTable;
        Table m_txErrConfigTable;
        // map<sai_object_id_t, string> m_portsStringsMap;
        map<string, string> m_portsStringsMap;

        // counetrs of tx errors
        map<string, uint64_t> m_txErrStatByPort;
        map<string, uint64_t> m_prevTxCounters;
        map<string, uint64_t> m_currTxCounters;

        map<string, PortState> m_portsState;

        uint64_t m_threshold = TX_DEFAULT_THRESHOLD;
        int m_poolingPeriod = TX_DEFAULT_POOLING_PERIOD;

        bool isPortMapInitialized = false;

        bool createPortNameMap();
        void initTimer();
        bool getTXStatistics();
        bool getTXCountersByAlias(string portAlias);
        void updatePortsState();
        bool handleSetCommand(const string& key, const vector<FieldValueTuple>& data);
        void setPoolingPeriod(const vector<FieldValueTuple>& data);
        void setThreshold(const vector<FieldValueTuple>& data);
        void insertStateToDb(string portAlias, PortState state);
        void setDefaultConfigParam();
};

#endif

