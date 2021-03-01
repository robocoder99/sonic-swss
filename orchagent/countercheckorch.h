#ifndef COUNTERCHECK_ORCH_H
#define COUNTERCHECK_ORCH_H

#include "debugcounterorch.h"

#include "orch.h"
#include "port.h"
#include "timer.h"
#include <array>

#define PFC_WD_TC_MAX 8

extern "C" {
#include "sai.h"
}

typedef std::vector<uint64_t> QueueMcCounters;
typedef std::array<uint64_t, PFC_WD_TC_MAX> PfcFrameCounters;

class CounterCheckOrch: public Orch
{
public:
    static CounterCheckOrch& getInstance(swss::DBConnector *db = nullptr);
    virtual void doTask(swss::SelectableTimer &timer);
    virtual void doTask(Consumer &consumer) {}
    void addPort(const swss::Port& port);
    void removePort(const swss::Port& port);
    void addDebugCounter(const std::string counter_name, const std::string counter_type, int threshold, int timeout);
    int removeDebugCounter(const std::string counter_name);

private:
    CounterCheckOrch(swss::DBConnector *db, std::vector<std::string> &tableNames);
    virtual ~CounterCheckOrch(void);
    QueueMcCounters getQueueMcCounters(const swss::Port& port);
    PfcFrameCounters getPfcFrameCounters(sai_object_id_t portId);
    void mcCounterCheck();
    void pfcFrameCounterCheck();
    void debugCounterCheck();

    void addPortAlarm(const std::string dev_name, const std::string counter_name);
    void removePortAlarm(const std::string dev_name, const std::string counter_name);

    void processPortCounters(const std::string counter_name, const std::string stat_name, const std::string dev_name, const std::string oid);
    std::vector<swss::FieldValueTuple> getDevices(std::string counter_type);
    std::string getStatName(const std::string counter_name, const std::string counter_type);
    
    int interval_count;

    std::map<std::string, std::string> m_stdPortMap;

    std::map<sai_object_id_t, QueueMcCounters> m_mcCountersMap;
    std::map<sai_object_id_t, PfcFrameCounters> m_pfcFrameCountersMap;

    // Should probably typdedef this for clarity
    std::map<std::string, std::tuple<std::string, int, int, std::map<std::string, long int>, std::map<std::string, int>>> debugCounterMap;

    std::shared_ptr<swss::DBConnector> m_countersDb = nullptr;
    std::shared_ptr<swss::DBConnector> m_asicDb = nullptr;
    std::shared_ptr<swss::DBConnector> m_applDb = nullptr;
    std::shared_ptr<swss::Table> m_countersTable = nullptr;
    std::shared_ptr<swss::Table> m_countersSwitchStatTable = nullptr;
    std::shared_ptr<swss::Table> m_countersPortStatTable = nullptr;
    std::shared_ptr<swss::Table> m_countersPortNameTable = nullptr;
    std::shared_ptr<swss::Table> m_asicStateTable = nullptr;
    std::shared_ptr<swss::Table> m_applPortTable = nullptr;
};

#endif
