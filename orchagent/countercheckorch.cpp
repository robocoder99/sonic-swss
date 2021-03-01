#include "countercheckorch.h"
#include "portsorch.h"
#include "select.h"
#include "notifier.h"
#include "redisclient.h"
#include "sai_serialize.h"
#include <inttypes.h>

#define COUNTER_CHECK_POLL_INTERVAL_SEC    5
#define COUNTER_MAX_INTERVAL_COUNT         300

#define PFC_CHECK_INTERVAL_COUNT           60
#define MC_CHECK_INTERVAL_COUNT            60
#define DEBUG_CHECK_INTERVAL_COUNT         1

#define DROPSTAT_DIR "/tmp/dropstat/"

extern sai_port_api_t *sai_port_api;

extern PortsOrch *gPortsOrch;

CounterCheckOrch& CounterCheckOrch::getInstance(DBConnector *db)
{
    SWSS_LOG_ENTER();

    static vector<string> tableNames = {};
    static CounterCheckOrch *wd = new CounterCheckOrch(db, tableNames);

    return *wd;
}

CounterCheckOrch::CounterCheckOrch(DBConnector *db, vector<string> &tableNames):
    Orch(db, tableNames),
    m_countersDb(new DBConnector("COUNTERS_DB", 0)),
    m_asicDb(new DBConnector("ASIC_DB", 0)),
    m_applDb(new DBConnector("APPL_DB", 0)),
    m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE)),
    m_countersSwitchStatTable(new Table(m_countersDb.get(), COUNTERS_DEBUG_NAME_SWITCH_STAT_MAP)),
    m_countersPortStatTable(new Table(m_countersDb.get(), COUNTERS_DEBUG_NAME_PORT_STAT_MAP)),
    m_countersPortNameTable(new Table(m_countersDb.get(), COUNTERS_PORT_NAME_MAP)),
    m_asicStateTable(new Table(m_asicDb.get(), "ASIC_STATE:SAI_OBJECT_TYPE_SWITCH")),
    m_applPortTable(new Table(m_applDb.get(), APP_PORT_TABLE_NAME))
{
    SWSS_LOG_ENTER();

    auto interv = timespec { .tv_sec = COUNTER_CHECK_POLL_INTERVAL_SEC, .tv_nsec = 0 };
    auto timer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(timer, this, "MC_COUNTERS_POLL");
    Orch::addExecutor(executor);

    m_stdPortMap = {
        {"RX_ERR",   "SAI_PORT_STAT_IF_IN_ERRORS"},
        {"RX_DROPS", "SAI_PORT_STAT_IF_IN_DISCARDS"},
        {"TX_ERR",   "SAI_PORT_STAT_IF_OUT_ERRORS"},
        {"TX_DROPS", "SAI_PORT_STAT_IF_OUT_DISCARDS"},
        {"TX_OK",    "SAI_PORT_STAT_ETHER_STATS_TX_NO_ERRORS"}
    };

    interval_count = 0;
    timer->start();

}

CounterCheckOrch::~CounterCheckOrch(void)
{
    SWSS_LOG_ENTER();
}

void CounterCheckOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_ENTER();

    if(interval_count % MC_CHECK_INTERVAL_COUNT == 0) 
        mcCounterCheck();

    if(interval_count % PFC_CHECK_INTERVAL_COUNT == 0) 
        pfcFrameCounterCheck();

    if(interval_count % DEBUG_CHECK_INTERVAL_COUNT == 0) 
        debugCounterCheck();

    interval_count++;

    if(interval_count % COUNTER_MAX_INTERVAL_COUNT == 0)
        interval_count = 0;

}


std::string CounterCheckOrch::getStatName(const std::string counter_name, const std::string counter_type)
{
    vector<FieldValueTuple> fieldValues;
    // Get SAI stat name of counter
    if (counter_type.find("PORT") != std::string::npos)
    {
        m_countersPortStatTable->get("", fieldValues);

    }
    else if (counter_type.find("SWITCH") != std::string::npos)
    {
        m_countersSwitchStatTable->get("", fieldValues); 
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);

        if (field == counter_name)
        {
            return value;
        }
    }

    if (counter_type.empty())
    {
       return m_stdPortMap[counter_name];
    }

    return std::string();
}

void CounterCheckOrch::addPortAlarm(const std::string dev_name, const std::string counter_name)
{
    SWSS_LOG_ERROR("Counter %s out of bounds on port %s.", counter_name.c_str(), dev_name.c_str());

    if (dev_name == "Switch")
        return;

    vector<FieldValueTuple> fieldValues;
    m_applPortTable->get(dev_name, fieldValues);

    fieldValues.push_back(FieldValueTuple("alarm", counter_name));
    m_applPortTable->set(dev_name, fieldValues);

}

void CounterCheckOrch::removePortAlarm(const std::string dev_name, const std::string counter_name)
{
    if (dev_name == "Switch")
        return;

    vector<FieldValueTuple> fieldValues;
    m_applPortTable->get(dev_name, fieldValues);

    fieldValues.push_back(FieldValueTuple("alarm", ""));
    m_applPortTable->set(dev_name, fieldValues);
}

void CounterCheckOrch::processPortCounters(const std::string counter_name, const std::string stat_name, const std::string dev_name, const std::string oid)
{
    vector<FieldValueTuple> portData;
    m_countersTable->get(oid, portData);
    
    int last_count = 0;
    long int ts = 0;
    
    int& threshold = std::get<1>(debugCounterMap[counter_name]);
    int& timeout = std::get<2>(debugCounterMap[counter_name]);

    std::map<std::string, long int>& timestamp = std::get<3>(debugCounterMap[counter_name]);
    std::map<std::string, int>& lcounts = std::get<4>(debugCounterMap[counter_name]);
    
    long int now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    if ( lcounts.find(oid) != lcounts.end() )
    {
        last_count = lcounts[oid];
    }
    if ( timestamp.find(oid) != timestamp.end() )
    {
        ts = timestamp[oid];
    }

    for (const auto& pfv : portData)
    {
        const auto pfield = fvField(pfv);
        const auto pvalue = fvValue(pfv);

        if (pfield == stat_name)
        {
	    if( (now - ts) > timeout && timeout > 0 )
            {
                lcounts[oid] = stoi(pvalue); // Reset count
                timestamp[oid] = now;

                if ( (stoi(pvalue) - last_count) < threshold )
                {
            	    removePortAlarm(dev_name, counter_name);
                }
            }

	    if( (stoi(pvalue) - last_count) > threshold && threshold > 0)
            {
		addPortAlarm(dev_name, counter_name);
	    }

            break;
        }
    }
}

vector<FieldValueTuple> CounterCheckOrch::getDevices(std::string counter_type)
{ 
    if (counter_type.find("SWITCH") != std::string::npos)
    {
        vector<string> keys;
        m_asicStateTable->getKeys(keys);
        return {{"Switch", keys[0]}};
    }
        
    vector<FieldValueTuple> portNames;
    m_countersPortNameTable->get("", portNames);
    return portNames;
}

void CounterCheckOrch::debugCounterCheck()
{
    SWSS_LOG_ENTER();

    for (auto& i : debugCounterMap)
    {
        std::string counter_name = i.first;
        std::string counter_type;
        int threshold;
        int timeout;
        
        std::tie (counter_type, threshold, timeout, std::ignore, std::ignore) = i.second;

        if ( timeout < 0 && threshold < 0 )
        {
            continue;
        }

        std::string stat_name = getStatName(counter_name, counter_type);
        
        for (const auto& fv : getDevices(counter_type))
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);

            processPortCounters(counter_name, stat_name, field, value);
        }
    } 
}

void CounterCheckOrch::mcCounterCheck()
{
    SWSS_LOG_ENTER();

    for (auto& i : m_mcCountersMap)
    {
        auto oid = i.first;
        auto mcCounters = i.second;
        uint8_t pfcMask = 0;

        Port port;
        if (!gPortsOrch->getPort(oid, port))
        {
            SWSS_LOG_ERROR("Invalid port oid 0x%" PRIx64, oid);
            continue;
        }

        auto newMcCounters = getQueueMcCounters(port);

        if (!gPortsOrch->getPortPfc(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC mask on port %s", port.m_alias.c_str());
            continue;
        }

        for (size_t prio = 0; prio != mcCounters.size(); prio++)
        {
            bool isLossy = ((1 << prio) & pfcMask) == 0;
            if (newMcCounters[prio] == numeric_limits<uint64_t>::max())
            {
                SWSS_LOG_WARN("Could not retreive MC counters on queue %zu port %s",
                        prio,
                        port.m_alias.c_str());
            }
            else if (!isLossy && mcCounters[prio] < newMcCounters[prio])
            {
                SWSS_LOG_WARN("Got Multicast %" PRIu64 " frame(s) on lossless queue %zu port %s",
                        newMcCounters[prio] - mcCounters[prio],
                        prio,
                        port.m_alias.c_str());
            }
        }

        i.second= newMcCounters;
    }
}

void CounterCheckOrch::pfcFrameCounterCheck()
{
    SWSS_LOG_ENTER();

    for (auto& i : m_pfcFrameCountersMap)
    {
        auto oid = i.first;
        auto counters = i.second;
        auto newCounters = getPfcFrameCounters(oid);
        uint8_t pfcMask = 0;

        Port port;
        if (!gPortsOrch->getPort(oid, port))
        {
            SWSS_LOG_ERROR("Invalid port oid 0x%" PRIx64, oid);
            continue;
        }

        if (!gPortsOrch->getPortPfc(port.m_port_id, &pfcMask))
        {
            SWSS_LOG_ERROR("Failed to get PFC mask on port %s", port.m_alias.c_str());
            continue;
        }

        for (size_t prio = 0; prio != counters.size(); prio++)
        {
            bool isLossy = ((1 << prio) & pfcMask) == 0;
            if (newCounters[prio] == numeric_limits<uint64_t>::max())
            {
                SWSS_LOG_WARN("Could not retreive PFC frame count on queue %zu port %s",
                        prio,
                        port.m_alias.c_str());
            }
            else if (isLossy && counters[prio] < newCounters[prio])
            {
                SWSS_LOG_WARN("Got PFC %" PRIu64 " frame(s) on lossy queue %zu port %s",
                        newCounters[prio] - counters[prio],
                        prio,
                        port.m_alias.c_str());
            }
        }

        i.second = newCounters;
    }
}


PfcFrameCounters CounterCheckOrch::getPfcFrameCounters(sai_object_id_t portId)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fieldValues;
    PfcFrameCounters counters;
    counters.fill(numeric_limits<uint64_t>::max());

    static const array<string, PFC_WD_TC_MAX> counterNames =
    {
        "SAI_PORT_STAT_PFC_0_RX_PKTS",
        "SAI_PORT_STAT_PFC_1_RX_PKTS",
        "SAI_PORT_STAT_PFC_2_RX_PKTS",
        "SAI_PORT_STAT_PFC_3_RX_PKTS",
        "SAI_PORT_STAT_PFC_4_RX_PKTS",
        "SAI_PORT_STAT_PFC_5_RX_PKTS",
        "SAI_PORT_STAT_PFC_6_RX_PKTS",
        "SAI_PORT_STAT_PFC_7_RX_PKTS"
    };

    if (!m_countersTable->get(sai_serialize_object_id(portId), fieldValues))
    {
        return move(counters);
    }

    for (const auto& fv : fieldValues)
    {
        const auto field = fvField(fv);
        const auto value = fvValue(fv);


        for (size_t prio = 0; prio != counterNames.size(); prio++)
        {
            if (field == counterNames[prio])
            {
                counters[prio] = stoul(value);
            }
        }
    }

    return move(counters);
}

QueueMcCounters CounterCheckOrch::getQueueMcCounters(
        const Port& port)
{
    SWSS_LOG_ENTER();

    vector<FieldValueTuple> fieldValues;
    QueueMcCounters counters;
    RedisClient redisClient(m_countersDb.get());

    for (uint8_t prio = 0; prio < port.m_queue_ids.size(); prio++)
    {
        sai_object_id_t queueId = port.m_queue_ids[prio];
        auto queueIdStr = sai_serialize_object_id(queueId);
        auto queueType = redisClient.hget(COUNTERS_QUEUE_TYPE_MAP, queueIdStr);

        if (queueType.get() == nullptr || *queueType != "SAI_QUEUE_TYPE_MULTICAST" || !m_countersTable->get(queueIdStr, fieldValues))
        {
            continue;
        }

        uint64_t pkts = numeric_limits<uint64_t>::max();
        for (const auto& fv : fieldValues)
        {
            const auto field = fvField(fv);
            const auto value = fvValue(fv);

            if (field == "SAI_QUEUE_STAT_PACKETS")
            {
                pkts = stoul(value);
            }
        }
        counters.push_back(pkts);
    }

    return move(counters);
}

void CounterCheckOrch::addDebugCounter(const std::string counter_name, const std::string counter_type, int threshold, int timeout)
{
    auto it = debugCounterMap.find(counter_name);
    if(it == debugCounterMap.end())
    {
        debugCounterMap.emplace(counter_name, 
                 std::tuple<std::string, int, int, std::map<std::string, long int>, std::map<std::string, int>>{
                     counter_type, 
                     threshold, 
                     timeout, 
                     {},
                     {}
                 });
    }
    else
    {
        auto& dat = debugCounterMap[counter_name];
        std::get<1>(dat) = threshold;
        std::get<2>(dat) = timeout;
    }
}

int CounterCheckOrch::removeDebugCounter(const std::string counter_name)
{
    auto it = debugCounterMap.find(counter_name);
    if(it != debugCounterMap.end())
    {
        debugCounterMap.erase(it); 
        return 1;
    }
    return 0;
}

void CounterCheckOrch::addPort(const Port& port)
{
    m_mcCountersMap.emplace(port.m_port_id, getQueueMcCounters(port));
    m_pfcFrameCountersMap.emplace(port.m_port_id, getPfcFrameCounters(port.m_port_id));
}

void CounterCheckOrch::removePort(const Port& port)
{
    m_mcCountersMap.erase(port.m_port_id);
    m_pfcFrameCountersMap.erase(port.m_port_id);
}
