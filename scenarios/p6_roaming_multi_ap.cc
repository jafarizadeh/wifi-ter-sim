// scratch/p6_roaming_multi_ap.cc
// Part 6: Multi-AP roaming under traffic (Wi-Fi + CSMA), realistic channel, reproducible outputs.

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/propagation-module.h"
#include "ns3/system-path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

// ---------- small IO helpers ----------
static void
MakeDirs(const std::string &outDir)
{
    SystemPath::MakeDirectories(outDir);
    SystemPath::MakeDirectories(outDir + "/raw");
    SystemPath::MakeDirectories(outDir + "/logs");
    SystemPath::MakeDirectories(outDir + "/plots");
}

static bool
IsFileNonEmpty(const std::string &path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    return f.good() && (f.peek() != std::ifstream::traits_type::eof());
}

static std::string
ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------- RTT probe (UDP timestamp echo) ----------
static inline void
WriteU64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; --i)
    {
        p[i] = static_cast<uint8_t>(v & 0xff);
        v >>= 8;
    }
}
static inline uint64_t
ReadU64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}
static inline void
WriteU32(uint8_t *p, uint32_t v)
{
    for (int i = 3; i >= 0; --i)
    {
        p[i] = static_cast<uint8_t>(v & 0xff);
        v >>= 8;
    }
}
static inline uint32_t
ReadU32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v = (v << 8) | p[i];
    return v;
}

class RttEchoServer : public Application
{
public:
    void Setup(uint16_t port) { m_port = port; }

    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        m_socket->SetRecvCallback(MakeCallback(&RttEchoServer::HandleRead, this));
    }

    void StopApplication() override
    {
        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
    }

private:
    void HandleRead(Ptr<Socket> s)
    {
        Address from;
        Ptr<Packet> p;
        while ((p = s->RecvFrom(from)))
        {
            s->SendTo(p, 0, from);
        }
    }

    Ptr<Socket> m_socket;
    uint16_t m_port{9000};
};

class RttCsvProbe : public Application
{
public:
    void Setup(Ipv4Address peerIp, uint16_t peerPort, Time interval, const std::string &csvPath)
    {
        m_peerIp = peerIp;
        m_peerPort = peerPort;
        m_interval = interval;
        m_csvPath = csvPath;
    }

    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(InetSocketAddress(m_peerIp, m_peerPort));
        m_socket->SetRecvCallback(MakeCallback(&RttCsvProbe::HandleRead, this));

        m_csv.open(m_csvPath, std::ios::out | std::ios::trunc);
        if (m_csv.is_open())
        {
            m_csv << "time_s,seq,rtt_ms\n";
            m_csv.flush();
        }

        m_seq = 0;
        m_running = true;
        SendOne();
    }

    void StopApplication() override
    {
        m_running = false;
        if (m_ev.IsPending())
            Simulator::Cancel(m_ev);

        if (m_socket)
        {
            m_socket->Close();
            m_socket = nullptr;
        }
        if (m_csv.is_open())
            m_csv.close();
    }

private:
    void SendOne()
    {
        if (!m_running)
            return;

        uint8_t buf[56] = {0};
        const uint64_t txNs = static_cast<uint64_t>(Simulator::Now().GetNanoSeconds());
        WriteU64(buf, txNs);
        WriteU32(buf + 8, m_seq);

        Ptr<Packet> p = Create<Packet>(buf, sizeof(buf));
        m_socket->Send(p);

        m_ev = Simulator::Schedule(m_interval, &RttCsvProbe::SendOne, this);
        ++m_seq;
    }

    void HandleRead(Ptr<Socket> s)
    {
        Address from;
        Ptr<Packet> p;
        while ((p = s->RecvFrom(from)))
        {
            if (p->GetSize() < 12)
                continue;

            std::vector<uint8_t> buf(p->GetSize());
            p->CopyData(buf.data(), buf.size());

            const uint64_t txNs = ReadU64(buf.data());
            const uint32_t seq = ReadU32(buf.data() + 8);

            Time tx = NanoSeconds(static_cast<int64_t>(txNs));
            Time rtt = Simulator::Now() - tx;

            if (m_csv.is_open())
            {
                const double rttMs = rtt.GetNanoSeconds() / 1e6;
                m_csv << std::fixed << std::setprecision(6)
                      << Simulator::Now().GetSeconds() << ","
                      << seq << ","
                      << rttMs << "\n";

                m_csv.flush();
            }
        }
    }

    Ptr<Socket> m_socket;
    EventId m_ev;
    bool m_running{false};

    Ipv4Address m_peerIp;
    uint16_t m_peerPort{9000};
    Time m_interval{MilliSeconds(200)};
    uint32_t m_seq{0};

    std::string m_csvPath;
    std::ofstream m_csv;
};

// ---------- Throughput sampler ----------
class ThroughputSampler
{
public:
    void Init(Ptr<PacketSink> sink, const std::string &path, double intervalS, bool append)
    {
        m_sink = sink;
        m_path = path;
        m_interval = intervalS;
        m_lastRx = 0;

        const bool needHeader = !IsFileNonEmpty(m_path);
        m_of.open(m_path, std::ios::out | (append ? std::ios::app : std::ios::trunc));
        if (m_of.is_open() && needHeader)
        {
            m_of << "time_s,throughput_bps\n";
            m_of.flush();
        }
    }

    void StartAt(double t0) { Simulator::Schedule(Seconds(t0), &ThroughputSampler::Tick, this); }

    void Stop()
    {
        if (m_of.is_open())
            m_of.close();
    }

private:
    void Tick()
    {
        const double now = Simulator::Now().GetSeconds();
        const uint64_t cur = m_sink ? m_sink->GetTotalRx() : 0;
        const uint64_t diff = (cur >= m_lastRx) ? (cur - m_lastRx) : 0;
        const double thr = (8.0 * static_cast<double>(diff)) / m_interval;

        if (m_of.is_open())
        {
            m_of << std::fixed << std::setprecision(6) << now << "," << thr << "\n";
            m_of.flush();
        }

        m_lastRx = cur;
        Simulator::Schedule(Seconds(m_interval), &ThroughputSampler::Tick, this);
    }

    Ptr<PacketSink> m_sink;
    std::string m_path;
    std::ofstream m_of;
    double m_interval{0.5};
    uint64_t m_lastRx{0};
};

// ---------- Position logger ----------
class PositionLogger
{
public:
    void Init(Ptr<MobilityModel> mob, const std::string &path, double intervalS)
    {
        m_mob = mob;
        m_path = path;
        m_interval = intervalS;

        m_of.open(m_path, std::ios::out | std::ios::trunc);
        if (m_of.is_open())
        {
            m_of << "time_s,x,y,z\n";
            m_of.flush();
        }
    }

    void StartAt(double t0) { Simulator::Schedule(Seconds(t0), &PositionLogger::Tick, this); }

    void Stop()
    {
        if (m_of.is_open())
            m_of.close();
    }

private:
    void Tick()
    {
        const double now = Simulator::Now().GetSeconds();
        Vector p = m_mob ? m_mob->GetPosition() : Vector();

        if (m_of.is_open())
        {
            m_of << std::fixed << std::setprecision(6)
                 << now << "," << p.x << "," << p.y << "," << p.z << "\n";
            m_of.flush();
        }

        Simulator::Schedule(Seconds(m_interval), &PositionLogger::Tick, this);
    }

    Ptr<MobilityModel> m_mob;
    std::string m_path;
    std::ofstream m_of;
    double m_interval{0.2};
};

// ---------- Dynamic routing controller (keep reachability across roaming) ----------
class RoamRoutingController : public Object
{
public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::RoamRoutingController")
                                .SetParent<Object>()
                                .AddConstructor<RoamRoutingController>();
        return tid;
    }

    RoamRoutingController() = default;
    ~RoamRoutingController() override = default;
    void Setup(Ptr<Node> sta,
               Ptr<Node> server,
               Ptr<Node> ap1,
               Ptr<Node> ap2,
               Ptr<NetDevice> staDev,
               Ptr<NetDevice> serverCsmaDev,
               Ipv4Address staIp,
               Ipv4Address ap1WifiIp,
               Ipv4Address ap2WifiIp,
               Ipv4Address ap1CsmaIp,
               Ipv4Address ap2CsmaIp,
               Mac48Address ap1Bssid,
               Mac48Address ap2Bssid)
    {
        m_sta = sta;
        m_server = server;
        m_ap1 = ap1;
        m_ap2 = ap2;
        m_staDev = staDev;
        m_serverCsmaDev = serverCsmaDev;

        m_staIp = staIp;
        m_ap1WifiIp = ap1WifiIp;
        m_ap2WifiIp = ap2WifiIp;
        m_ap1CsmaIp = ap1CsmaIp;
        m_ap2CsmaIp = ap2CsmaIp;
        m_ap1Bssid = ap1Bssid;
        m_ap2Bssid = ap2Bssid;

        Ipv4StaticRoutingHelper h;
        m_staSr = h.GetStaticRouting(m_sta->GetObject<Ipv4>());
        m_serverSr = h.GetStaticRouting(m_server->GetObject<Ipv4>());
        m_ap1Sr = h.GetStaticRouting(m_ap1->GetObject<Ipv4>());
        m_ap2Sr = h.GetStaticRouting(m_ap2->GetObject<Ipv4>());
    }

    void UpdateForBssid(Mac48Address bssid)
    {
        if (bssid == Mac48Address())
            return;

        bool toAp1 = (bssid == m_ap1Bssid);
        bool toAp2 = (bssid == m_ap2Bssid);
        if (!toAp1 && !toAp2)
            return;

        if (!m_hasLast)
        {
            m_lastIsAp1 = toAp1;
            m_hasLast = true;
        }
        else if (m_lastIsAp1 == toAp1)
        {
            return;
        }
        m_lastIsAp1 = toAp1;

        Ipv4Address gwWifi = toAp1 ? m_ap1WifiIp : m_ap2WifiIp;
        Ipv4Address gwCsma = toAp1 ? m_ap1CsmaIp : m_ap2CsmaIp;

        ApplyStaRoute(gwWifi);
        ApplyServerRoute(gwCsma);
        ApplyNonServingApHostRoute(toAp1);
    }

private:
    void RemoveMatchingNetworkRoutes(Ptr<Ipv4StaticRouting> sr, Ipv4Address net, Ipv4Mask mask)
    {
        for (int i = static_cast<int>(sr->GetNRoutes()) - 1; i >= 0; --i)
        {
            Ipv4RoutingTableEntry e = sr->GetRoute(static_cast<uint32_t>(i));
            if (e.IsNetwork() && e.GetDestNetwork() == net && e.GetDestNetworkMask() == mask)
                sr->RemoveRoute(static_cast<uint32_t>(i));
        }
    }

    void RemoveMatchingHostRoutes(Ptr<Ipv4StaticRouting> sr, Ipv4Address host)
    {
        for (int i = static_cast<int>(sr->GetNRoutes()) - 1; i >= 0; --i)
        {
            Ipv4RoutingTableEntry e = sr->GetRoute(static_cast<uint32_t>(i));
            if (e.IsHost() && e.GetDest() == host)
                sr->RemoveRoute(static_cast<uint32_t>(i));
        }
    }

    void ApplyStaRoute(Ipv4Address gwWifi)
    {
        Ptr<Ipv4> ipv4 = m_sta->GetObject<Ipv4>();
        uint32_t ifIndex = ipv4->GetInterfaceForDevice(m_staDev);

        RemoveMatchingNetworkRoutes(m_staSr, Ipv4Address("10.2.0.0"), Ipv4Mask("255.255.255.0"));
        m_staSr->AddNetworkRouteTo(Ipv4Address("10.2.0.0"), Ipv4Mask("255.255.255.0"), gwWifi, ifIndex);
    }

    void ApplyServerRoute(Ipv4Address gwCsma)
    {
        Ptr<Ipv4> ipv4 = m_server->GetObject<Ipv4>();
        uint32_t ifIndex = ipv4->GetInterfaceForDevice(m_serverCsmaDev);

        RemoveMatchingHostRoutes(m_serverSr, m_staIp);
        m_serverSr->AddHostRouteTo(m_staIp, gwCsma, ifIndex);
    }

    void ApplyNonServingApHostRoute(bool servingIsAp1)
    {
        Ptr<Node> nonServing = servingIsAp1 ? m_ap2 : m_ap1;
        Ptr<Ipv4StaticRouting> sr = servingIsAp1 ? m_ap2Sr : m_ap1Sr;
        Ipv4Address via = servingIsAp1 ? m_ap1CsmaIp : m_ap2CsmaIp;

        Ptr<Ipv4> ipv4 = nonServing->GetObject<Ipv4>();
        uint32_t outIf = 0;
        for (uint32_t i = 0; i < ipv4->GetNInterfaces(); ++i)
        {
            for (uint32_t a = 0; a < ipv4->GetNAddresses(i); ++a)
            {
                Ipv4InterfaceAddress ia = ipv4->GetAddress(i, a);
                if (ia.GetLocal().CombineMask(Ipv4Mask("255.255.255.0")) == Ipv4Address("10.2.0.0"))
                    outIf = i;
            }
        }

        RemoveMatchingHostRoutes(sr, m_staIp);
        sr->AddHostRouteTo(m_staIp, via, outIf);
    }

    Ptr<Node> m_sta, m_server, m_ap1, m_ap2;
    Ptr<NetDevice> m_staDev, m_serverCsmaDev;

    Ipv4Address m_staIp;
    Ipv4Address m_ap1WifiIp, m_ap2WifiIp;
    Ipv4Address m_ap1CsmaIp, m_ap2CsmaIp;
    Mac48Address m_ap1Bssid, m_ap2Bssid;

    Ptr<Ipv4StaticRouting> m_staSr, m_serverSr, m_ap1Sr, m_ap2Sr;

    bool m_hasLast{false};
    bool m_lastIsAp1{true};
};

// ---------- Roaming logger (polling BSSID) ----------
class RoamingLogger
{
public:
    void Init(Ptr<WifiMac> mac, uint8_t linkId, const std::string &path, double pollS, bool append)
    {
        m_mac = mac;
        m_linkId = linkId;
        m_path = path;
        m_poll = Seconds(pollS);

        const bool needHeader = !IsFileNonEmpty(m_path);
        m_of.open(m_path, std::ios::out | (append ? std::ios::app : std::ios::trunc));
        if (m_of.is_open() && needHeader)
        {
            m_of << "time_s,type,bssid\n";
            m_of.flush();
        }

        m_have = false;
        m_firstRoam = -1.0;
    }

    void SetRoutingController(Ptr<RoamRoutingController> rc) { m_rc = rc; }

    void StartAt(double t0) { Simulator::Schedule(Seconds(t0), &RoamingLogger::Poll, this); }

    void Stop()
    {
        if (m_of.is_open())
            m_of.close();
    }

    double GetFirstRoamTime() const { return m_firstRoam; }

private:
    void Poll()
    {
        if (m_mac)
        {
            const Mac48Address cur = m_mac->GetBssid(m_linkId);

            if (!m_have)
            {
                m_have = true;
                m_last = cur;
                if (m_of.is_open())
                {
                    m_of << std::fixed << std::setprecision(6)
                         << Simulator::Now().GetSeconds() << ",INIT," << cur << "\n";
                    m_of.flush();
                }
                if (m_rc)
                    m_rc->UpdateForBssid(cur);
            }
            else if (cur != m_last)
            {
                const double t = Simulator::Now().GetSeconds();
                if (m_firstRoam < 0.0)
                    m_firstRoam = t;

                if (m_of.is_open())
                {
                    m_of << std::fixed << std::setprecision(6)
                         << t << ",ROAM," << cur << "\n";
                    m_of.flush();
                }

                m_last = cur;
                if (m_rc)
                    m_rc->UpdateForBssid(cur);
            }
        }

        Simulator::Schedule(m_poll, &RoamingLogger::Poll, this);
    }

    Ptr<WifiMac> m_mac;
    uint8_t m_linkId{0};
    std::string m_path;
    std::ofstream m_of;
    Time m_poll{MilliSeconds(50)};
    bool m_have{false};
    Mac48Address m_last;
    double m_firstRoam{-1.0};

    Ptr<RoamRoutingController> m_rc;
};
// ---------- Realistic best-AP roamer (RSSI-based + hysteresis + dwell) ----------
class BestApRoamer : public Object
{
public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::BestApRoamer")
                .SetParent<Object>()
                .SetGroupName("Wifi")
                .AddConstructor<BestApRoamer>();
        return tid;
    }

    BestApRoamer() = default;
    ~BestApRoamer() override = default;

    // Keep the same signature you already use in main (even if some fields are not used now).
    void Setup(Ptr<MobilityModel> sta,
               Ptr<MobilityModel> ap1,
               Ptr<MobilityModel> ap2,
               Ptr<WifiMac> staMac,
               uint8_t linkId,
               Mac48Address ap1Bssid,
               Mac48Address ap2Bssid,
               Ptr<ApWifiMac> ap1Mac,
               Ptr<ApWifiMac> ap2Mac,
               Mac48Address staAddr,
               double txAp1Dbm,
               double txAp2Dbm,
               double logExp,
               double refLossDb,
               double checkS,
               double hystDb,
               double dwellS,
               double minGapS)
    {
        m_sta = sta;
        m_ap1 = ap1;
        m_ap2 = ap2;
        m_staMac = staMac;
        m_linkId = linkId;

        m_ap1Bssid = ap1Bssid;
        m_ap2Bssid = ap2Bssid;

        m_ap1Mac = ap1Mac;
        m_ap2Mac = ap2Mac;
        m_staAddr = staAddr;

        m_txAp1Dbm = txAp1Dbm;
        m_txAp2Dbm = txAp2Dbm;
        m_logExp = logExp;
        m_refLossDb = refLossDb;

        // Defensive: avoid zero/negative periods
        m_check = Seconds(std::max(1e-6, checkS));
        m_hystDb = hystDb;
        m_dwellS = std::max(0.0, dwellS);
        m_minGapS = std::max(0.0, minGapS);
    }

    void SetAssocManager(Ptr<WifiAssocManager> am)
    {
        m_assocMgr = am;
    }

    // IMPORTANT: schedule using Ptr<> so the event keeps this object alive.
    void StartAt(double t0)
    {
        Ptr<BestApRoamer> self = GetObject<BestApRoamer>();
        Simulator::Schedule(Seconds(std::max(0.0, t0)), &BestApRoamer::Tick, self);
    }

private:
    void DoDispose() override
    {
        // Release references (good hygiene; not strictly required)
        m_sta = nullptr;
        m_ap1 = nullptr;
        m_ap2 = nullptr;
        m_staMac = nullptr;
        m_ap1Mac = nullptr;
        m_ap2Mac = nullptr;
        m_assocMgr = nullptr;
        Object::DoDispose();
    }
    bool m_haveLastBssid{false};
    Mac48Address m_lastBssid{};

    // Simple log-distance RSSI estimate (no shadowing/fading here; those are in PHY anyway).
    double EstimateRxDbm(double txDbm, Ptr<MobilityModel> tx, Ptr<MobilityModel> rx) const
    {
        double d = tx->GetDistanceFrom(rx);
        if (d < 0.1)
            d = 0.1;

        // ReferenceDistance = 1 m
        const double pathLossDb = m_refLossDb + 10.0 * m_logExp * std::log10(d / 1.0);
        return txDbm - pathLossDb;
    }

    // Trigger an active scan via the association manager.
    void TriggerScan()
    {
        if (!m_assocMgr)
        {
            NS_LOG_UNCOND("[P6] WARN: assoc manager is null; cannot trigger scan");
            return;
        }

        ns3::WifiScanParams sp;

        sp.probeDelay = MicroSeconds(0);
        sp.minChannelTime = MilliSeconds(30);
        sp.maxChannelTime = MilliSeconds(60);

        const std::size_t need = static_cast<std::size_t>(m_linkId) + 1u;
        if (sp.channelList.size() < need)
        {
            sp.channelList.resize(need);
        }

        NS_LOG_UNCOND("[P6] Trigger roam: StartScanning()"
                      << " linkId=" << unsigned(m_linkId)
                      << " channelList.size=" << sp.channelList.size());

        m_assocMgr->StartScanning(std::move(sp));
    }

    void Tick()
    {
        Ptr<BestApRoamer> self = GetObject<BestApRoamer>(); // keep-alive for reschedule

        // If not fully initialized yet, just reschedule.
        if (!m_sta || !m_ap1 || !m_ap2 || !m_staMac)
        {
            Simulator::Schedule(m_check, &BestApRoamer::Tick, self);
            return;
        }

        const double now = Simulator::Now().GetSeconds();
        const Mac48Address cur = m_staMac->GetBssid(m_linkId);

        const bool servingIsAp1 = (cur == m_ap1Bssid);
        const bool servingIsAp2 = (cur == m_ap2Bssid);

        if (!m_haveLastBssid)
        {
            m_lastBssid = cur;
            m_haveLastBssid = true;
        }
        else if (cur != m_lastBssid)
        {
            NS_LOG_UNCOND("[P6] Roam happened: BSSID " << m_lastBssid
                                                       << " -> " << cur
                                                       << " at t=" << now << "s");
            m_lastBssid = cur;
        }

        // Not associated to any target AP yet -> do nothing
        if (!servingIsAp1 && !servingIsAp2)
        {
            m_candidate = false;
            Simulator::Schedule(m_check, &BestApRoamer::Tick, self);
            return;
        }

        const double rx1 = EstimateRxDbm(m_txAp1Dbm, m_ap1, m_sta);
        const double rx2 = EstimateRxDbm(m_txAp2Dbm, m_ap2, m_sta);

        // Decide whether the other AP is better by hysteresis margin.
        bool preferAp2 = false;
        if (servingIsAp1)
        {
            preferAp2 = (rx2 > rx1 + m_hystDb);
        }
        else // servingIsAp2
        {
            preferAp2 = !(rx1 > rx2 + m_hystDb); // i.e., AP2 remains preferred unless AP1 is clearly better
        }

        // Enforce a minimum time gap between scan triggers.
        if (m_lastTriggerTime >= 0.0 && (now - m_lastTriggerTime) < m_minGapS)
        {
            Simulator::Schedule(m_check, &BestApRoamer::Tick, self);
            return;
        }

        const bool roamCondition =
            (servingIsAp1 && preferAp2) ||
            (servingIsAp2 && !preferAp2);

        if (roamCondition)
        {
            if (!m_candidate)
            {
                // Start dwell timer
                m_candidate = true;
                m_candidateStart = now;
            }
            else
            {
                // If condition stays true long enough -> trigger scan
                if ((now - m_candidateStart) >= m_dwellS)
                {
                    TriggerScan();
                    m_lastTriggerTime = now;
                    m_candidate = false;
                }
            }
        }
        else
        {
            // Condition not satisfied -> reset dwell
            m_candidate = false;
        }

        Simulator::Schedule(m_check, &BestApRoamer::Tick, self);
    }

private:
    Ptr<MobilityModel> m_sta, m_ap1, m_ap2;
    Ptr<WifiMac> m_staMac;
    uint8_t m_linkId{0};

    Mac48Address m_ap1Bssid, m_ap2Bssid;
    Ptr<ApWifiMac> m_ap1Mac, m_ap2Mac;
    Mac48Address m_staAddr;

    Ptr<WifiAssocManager> m_assocMgr;

    double m_txAp1Dbm{20.0}, m_txAp2Dbm{16.0};
    double m_logExp{3.0};
    double m_refLossDb{46.6777};

    Time m_check{MilliSeconds(200)};
    double m_hystDb{4.0};
    double m_dwellS{1.0};
    double m_minGapS{2.0};

    bool m_candidate{false};
    double m_candidateStart{0.0};
    double m_lastTriggerTime{-1.0};
};

NS_OBJECT_ENSURE_REGISTERED(BestApRoamer);

// ---------- main ----------
int main(int argc, char *argv[])
{
    double simTime = 30.0;
    double appStart = 2.0;
    double moveStart = 5.0;

    double apDistance = 30.0;
    double staSpeed = 1.0;

    std::string ssidStr = "wifi6-ter";
    std::string outDir = "results/p6";

    bool pcap = false;
    bool flowmon = true;

    int seed = 1;
    int run = 1;

    int pktSize = 1200;
    std::string udpRate = "20Mbps";
    double interval = 0.5;

    double txPowerStaDbm = 16.0;
    double txPowerAp1Dbm = 20.0;
    double txPowerAp2Dbm = 16.0;
    double noiseFigureDb = 7.0;

    double logExp = 3.0;
    double shadowingSigmaDb = 4.0;
    bool enableFading = true;

    bool useMinstrel = true;
    std::string wifiStd = "ax"; // ax/ac/n

    double roamPollS = 0.05;
    uint32_t linkId = 0;

    double posPollS = 0.2;

    bool enableRttProbe = true;
    double rttHz = 5.0;
    uint16_t rttPort = 9000;

    bool activeProbing = false;
    bool bestRoam = true;
    double roamCheckS = 0.2;
    double roamHystDb = 4.0;
    double roamDwellS = 1.0;
    double roamMinGapS = 2.0;

    CommandLine cmd;
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("appStart", "Traffic start time (s)", appStart);
    cmd.AddValue("moveStart", "STA movement start time (s)", moveStart);

    cmd.AddValue("apDistance", "AP1-AP2 distance (m)", apDistance);
    cmd.AddValue("staSpeed", "STA speed (m/s)", staSpeed);

    cmd.AddValue("ssid", "Wi-Fi SSID (same on both APs)", ssidStr);
    cmd.AddValue("outDir", "Output directory", outDir);

    cmd.AddValue("pcap", "Enable PCAP", pcap);
    cmd.AddValue("flowmon", "Enable FlowMonitor", flowmon);

    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run", run);

    cmd.AddValue("pktSize", "UDP packet size (bytes)", pktSize);
    cmd.AddValue("udpRate", "UDP offered rate (e.g., 20Mbps)", udpRate);
    cmd.AddValue("interval", "Throughput sampling interval (s)", interval);

    cmd.AddValue("txPowerStaDbm", "STA Tx power (dBm)", txPowerStaDbm);
    cmd.AddValue("txPowerAp1Dbm", "AP1 Tx power (dBm)", txPowerAp1Dbm);
    cmd.AddValue("txPowerAp2Dbm", "AP2 Tx power (dBm)", txPowerAp2Dbm);
    cmd.AddValue("noiseFigureDb", "Rx noise figure (dB)", noiseFigureDb);

    cmd.AddValue("logExp", "LogDistance exponent", logExp);
    cmd.AddValue("shadowingSigmaDb", "Shadowing sigma (dB)", shadowingSigmaDb);
    cmd.AddValue("enableFading", "Enable Nakagami fading", enableFading);

    cmd.AddValue("useMinstrel", "Use MinstrelHtWifiManager", useMinstrel);
    cmd.AddValue("wifiStd", "Wi-Fi standard: ax|ac|n", wifiStd);

    cmd.AddValue("roamPoll", "BSSID polling interval (s)", roamPollS);
    cmd.AddValue("linkId", "Wifi linkId for GetBssid(linkId)", linkId);

    cmd.AddValue("posPoll", "STA position sampling interval (s)", posPollS);

    cmd.AddValue("enableRttProbe", "Enable RTT probe CSV", enableRttProbe);
    cmd.AddValue("rttHz", "RTT probe frequency (Hz)", rttHz);
    cmd.AddValue("rttPort", "RTT probe UDP port", rttPort);

    cmd.AddValue("activeProbing", "STA ActiveProbing (true/false)", activeProbing);
    cmd.AddValue("bestRoam", "Enable realistic best-AP roaming", bestRoam);
    cmd.AddValue("roamCheck", "Roam decision period (s)", roamCheckS);
    cmd.AddValue("roamHystDb", "Roam hysteresis (dB)", roamHystDb);
    cmd.AddValue("roamDwell", "Roam dwell time (s)", roamDwellS);
    cmd.AddValue("roamMinGap", "Min gap between roams (s)", roamMinGapS);

    cmd.Parse(argc, argv);

    LogComponentDisable("StaWifiMac", LOG_LEVEL_ALL);
    LogComponentDisable("WifiAssocManager", LOG_LEVEL_ALL);
    LogComponentDisable("WifiDefaultAssocManager", LOG_LEVEL_ALL);

    auto SetDefaultSafe = [](const std::string &name, const AttributeValue &v)
    {
        if (!Config::SetDefaultFailSafe(name, v))
        {
            NS_LOG_UNCOND("[P6] WARN: attribute not found: " << name);
        }
    };

    // ---- safer MAC/scan timers (keep your choices) ----
    SetDefaultSafe("ns3::StaWifiMac::AssocRequestTimeout", TimeValue(Seconds(0.5)));
    SetDefaultSafe("ns3::StaWifiMac::ProbeRequestTimeout", TimeValue(MilliSeconds(100)));
    SetDefaultSafe("ns3::StaWifiMac::WaitBeaconTimeout", TimeValue(MilliSeconds(200)));
    SetDefaultSafe("ns3::StaWifiMac::MaxMissedBeacons", UintegerValue(5));

    // ---- validate ----
    if (simTime <= 0.0 || appStart < 0.0 || appStart >= simTime || moveStart < 0.0 || moveStart >= simTime)
    {
        std::cerr << "ERROR: invalid simTime/appStart/moveStart\n";
        return 1;
    }
    if (apDistance <= 0.0 || staSpeed <= 0.0 || pktSize <= 0 || interval <= 0.0 || roamPollS <= 0.0 || posPollS <= 0.0)
    {
        std::cerr << "ERROR: invalid parameters\n";
        return 1;
    }
    if (enableRttProbe && rttHz <= 0.0)
    {
        std::cerr << "ERROR: invalid rttHz\n";
        return 1;
    }

    // ---- bestRoam implies not-too-fast polling (avoid log spam) ----
    if (bestRoam)
    {
        roamPollS = std::max(roamPollS, 0.2); // 200ms minimum
    }

    // ---- RNG ----
    RngSeedManager::SetSeed(static_cast<uint32_t>(seed));
    RngSeedManager::SetRun(static_cast<uint64_t>(run));

    // ---- output dirs ----
    MakeDirs(outDir);

    std::ostringstream tag;
    tag << "run" << run;

    const std::string roamPath = outDir + "/raw/roaming_events.txt";
    const std::string thrPath  = outDir + "/raw/throughput_timeseries.csv";
    const std::string sumPath  = outDir + "/raw/p6_summary.csv";

    const std::string roamRunPath = outDir + "/raw/roaming_events_" + tag.str() + ".txt";
    const std::string thrRunPath  = outDir + "/raw/throughput_timeseries_" + tag.str() + ".csv";
    const std::string posRunPath  = outDir + "/raw/sta_pos_" + tag.str() + ".csv";
    const std::string rttRunPath  = outDir + "/raw/rtt_probe_" + tag.str() + ".csv";
    const std::string pingTxtPath = outDir + "/logs/ping.txt";

    // ---- IMPORTANT FIX: reset global files (avoid mixing runs) ----
    {
        std::ofstream f(roamPath, std::ios::out | std::ios::trunc);
        f << "time_s,event,bssid\n";
    }
    {
        std::ofstream f(thrPath, std::ios::out | std::ios::trunc);
        f << "time_s,throughput_bps\n";
    }

    // ---- nodes ----
    Ptr<Node> staNode = CreateObject<Node>();
    Ptr<Node> ap1Node = CreateObject<Node>();
    Ptr<Node> ap2Node = CreateObject<Node>();
    Ptr<Node> serverNode = CreateObject<Node>();

    // ---- mobility ----
    MobilityHelper fixed;
    fixed.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    fixed.Install(NodeContainer(ap1Node, ap2Node, serverNode));
    ap1Node->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
    ap2Node->GetObject<MobilityModel>()->SetPosition(Vector(apDistance, 0.0, 0.0));
    serverNode->GetObject<MobilityModel>()->SetPosition(Vector(apDistance / 2.0, 1.0, 0.0));

    MobilityHelper staMob;
    staMob.SetMobilityModel("ns3::ConstantVelocityMobilityModel");
    staMob.Install(staNode);
    Ptr<ConstantVelocityMobilityModel> cv = staNode->GetObject<ConstantVelocityMobilityModel>();
    cv->SetPosition(Vector(2.0, 0.0, 0.0));
    cv->SetVelocity(Vector(0.0, 0.0, 0.0));

    Simulator::Schedule(Seconds(moveStart), [cv, staSpeed]()
                        { cv->SetVelocity(Vector(staSpeed, 0.0, 0.0)); });

    const double travel = (apDistance - 4.0) / staSpeed;
    const double stopMoveAt = std::min(simTime - 0.1, moveStart + std::max(0.0, travel));
    Simulator::Schedule(Seconds(stopMoveAt), [cv]()
                        { cv->SetVelocity(Vector(0.0, 0.0, 0.0)); });

    Ptr<MobilityModel> staMobModel = staNode->GetObject<MobilityModel>();

    // ---- propagation ----
    Ptr<LogDistancePropagationLossModel> logd = CreateObject<LogDistancePropagationLossModel>();
    logd->SetAttribute("Exponent", DoubleValue(logExp));
    logd->SetAttribute("ReferenceDistance", DoubleValue(1.0));
    logd->SetAttribute("ReferenceLoss", DoubleValue(46.6777));

    Ptr<NormalRandomVariable> normal = CreateObject<NormalRandomVariable>();
    normal->SetAttribute("Mean", DoubleValue(0.0));
    normal->SetAttribute("Variance", DoubleValue(shadowingSigmaDb * shadowingSigmaDb));

    Ptr<RandomPropagationLossModel> shadow = CreateObject<RandomPropagationLossModel>();
    shadow->SetAttribute("Variable", PointerValue(normal));
    logd->SetNext(shadow);

    if (enableFading)
    {
        Ptr<NakagamiPropagationLossModel> nak = CreateObject<NakagamiPropagationLossModel>();
        nak->SetAttribute("Distance1", DoubleValue(5.0));
        nak->SetAttribute("Distance2", DoubleValue(15.0));
        nak->SetAttribute("m0", DoubleValue(1.5));
        nak->SetAttribute("m1", DoubleValue(1.0));
        nak->SetAttribute("m2", DoubleValue(0.75));
        shadow->SetNext(nak);
    }

    Ptr<YansWifiChannel> ychan = CreateObject<YansWifiChannel>();
    ychan->SetPropagationDelayModel(CreateObject<ConstantSpeedPropagationDelayModel>());
    ychan->SetPropagationLossModel(logd);

    // ---- PHYs ----
    YansWifiPhyHelper phySta;
    phySta.SetChannel(ychan);
    phySta.Set("TxPowerStart", DoubleValue(txPowerStaDbm));
    phySta.Set("TxPowerEnd", DoubleValue(txPowerStaDbm));
    phySta.Set("TxPowerLevels", UintegerValue(1));
    phySta.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

    YansWifiPhyHelper phyAp1;
    phyAp1.SetChannel(ychan);
    phyAp1.Set("TxPowerStart", DoubleValue(txPowerAp1Dbm));
    phyAp1.Set("TxPowerEnd", DoubleValue(txPowerAp1Dbm));
    phyAp1.Set("TxPowerLevels", UintegerValue(1));
    phyAp1.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

    YansWifiPhyHelper phyAp2;
    phyAp2.SetChannel(ychan);
    phyAp2.Set("TxPowerStart", DoubleValue(txPowerAp2Dbm));
    phyAp2.Set("TxPowerEnd", DoubleValue(txPowerAp2Dbm));
    phyAp2.Set("TxPowerLevels", UintegerValue(1));
    phyAp2.Set("RxNoiseFigure", DoubleValue(noiseFigureDb));

    // ---- Wi-Fi helper ----
    WifiHelper wifi;
    const std::string w = ToLower(wifiStd);
    if (w == "ax")
        wifi.SetStandard(WIFI_STANDARD_80211ax);
    else if (w == "ac")
        wifi.SetStandard(WIFI_STANDARD_80211ac);
    else
        wifi.SetStandard(WIFI_STANDARD_80211n);

    if (useMinstrel)
        wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");
    else
        wifi.SetRemoteStationManager("ns3::IdealWifiManager");

    Ssid ssid = Ssid(ssidStr);

    WifiMacHelper macAp1;
    macAp1.SetType("ns3::ApWifiMac",
                   "Ssid", SsidValue(ssid),
                   "BeaconInterval", TimeValue(MicroSeconds(1024 * 100)));

    WifiMacHelper macAp2;
    macAp2.SetType("ns3::ApWifiMac",
                   "Ssid", SsidValue(ssid),
                   "BeaconInterval", TimeValue(MicroSeconds(1024 * 100)));

    NetDeviceContainer ap1Dev = wifi.Install(phyAp1, macAp1, ap1Node);
    NetDeviceContainer ap2Dev = wifi.Install(phyAp2, macAp2, ap2Node);

    Mac48Address ap1Bssid = Mac48Address::ConvertFrom(ap1Dev.Get(0)->GetAddress());
    Mac48Address ap2Bssid = Mac48Address::ConvertFrom(ap2Dev.Get(0)->GetAddress());

    WifiMacHelper macSta;
    macSta.SetType("ns3::StaWifiMac",
                   "Ssid", SsidValue(ssid),
                   "ActiveProbing", BooleanValue(activeProbing));

    NetDeviceContainer staDev = wifi.Install(phySta, macSta, staNode);

    Ptr<WifiNetDevice> staWifiDev = DynamicCast<WifiNetDevice>(staDev.Get(0));
    Ptr<WifiMac> staMacBase = staWifiDev ? staWifiDev->GetMac() : nullptr;

    Ptr<StaWifiMac> staMac = DynamicCast<StaWifiMac>(staMacBase);
    Ptr<WifiAssocManager> assocMgr = nullptr;

    if (staMac)
    {
        // keep consistent with CLI:
        staMac->SetAttribute("ActiveProbing", BooleanValue(activeProbing));

        Ptr<WifiDefaultAssocManager> am = CreateObject<WifiDefaultAssocManager>();
        am->SetStaWifiMac(staMac);
        staMac->SetAssocManager(am);
        assocMgr = am;
    }

    Ptr<WifiNetDevice> ap1WifiDev = DynamicCast<WifiNetDevice>(ap1Dev.Get(0));
    Ptr<WifiNetDevice> ap2WifiDev = DynamicCast<WifiNetDevice>(ap2Dev.Get(0));
    Ptr<ApWifiMac> ap1Mac = ap1WifiDev ? DynamicCast<ApWifiMac>(ap1WifiDev->GetMac()) : nullptr;
    Ptr<ApWifiMac> ap2Mac = ap2WifiDev ? DynamicCast<ApWifiMac>(ap2WifiDev->GetMac()) : nullptr;

    Mac48Address staAddr = Mac48Address::ConvertFrom(staDev.Get(0)->GetAddress());

    Ptr<BestApRoamer> roamer;
    if (bestRoam && staMacBase && ap1Mac && ap2Mac)
    {
        roamer = CreateObject<BestApRoamer>();
        roamer->Setup(staMobModel,
                      ap1Node->GetObject<MobilityModel>(),
                      ap2Node->GetObject<MobilityModel>(),
                      staMacBase,
                      static_cast<uint8_t>(linkId),
                      ap1Bssid,
                      ap2Bssid,
                      ap1Mac,
                      ap2Mac,
                      staAddr,
                      txPowerAp1Dbm,
                      txPowerAp2Dbm,
                      logExp,
                      46.6777,
                      roamCheckS,
                      roamHystDb,
                      roamDwellS,
                      roamMinGapS);

        roamer->SetAssocManager(assocMgr);

        // ---- FIX: never start roaming before movement + traffic is stable ----
        const double roamStart = std::max(appStart, moveStart) + 0.2;
        roamer->StartAt(std::max(1.0, roamStart));
    }

    // ---- CSMA backbone ----
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("1Gbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(50)));

    NodeContainer csmaNodes;
    csmaNodes.Add(ap1Node);
    csmaNodes.Add(ap2Node);
    csmaNodes.Add(serverNode);
    NetDeviceContainer csmaDevs = csma.Install(csmaNodes);

    // ---- IP stack ----
    InternetStackHelper internet;
    internet.Install(NodeContainer(staNode, ap1Node, ap2Node, serverNode));

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer staIf = ipv4.Assign(staDev);
    Ipv4InterfaceContainer ap1IfWifi = ipv4.Assign(ap1Dev);
    Ipv4InterfaceContainer ap2IfWifi = ipv4.Assign(ap2Dev);

    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer csmaIf = ipv4.Assign(csmaDevs);

    Ipv4Address staIp = staIf.GetAddress(0);
    Ipv4Address ap1WifiIp = ap1IfWifi.GetAddress(0);
    Ipv4Address ap2WifiIp = ap2IfWifi.GetAddress(0);

    Ipv4Address ap1CsmaIp = csmaIf.GetAddress(0);
    Ipv4Address ap2CsmaIp = csmaIf.GetAddress(1);
    Ipv4Address serverIp = csmaIf.GetAddress(2);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- apps: UDP traffic ----
    const uint16_t port = 5000;

    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(serverNode);
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTime));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));

    OnOffHelper onoff("ns3::UdpSocketFactory", InetSocketAddress(serverIp, port));
    onoff.SetAttribute("PacketSize", UintegerValue(static_cast<uint32_t>(pktSize)));
    onoff.SetAttribute("DataRate", StringValue(udpRate));
    onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
    onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    ApplicationContainer clientApp = onoff.Install(staNode);
    clientApp.Start(Seconds(appStart));
    clientApp.Stop(Seconds(simTime));

    // ---- RTT probe ----
    Ptr<RttEchoServer> rttSrv;
    Ptr<RttCsvProbe> rttCli;
    if (enableRttProbe)
    {
        rttSrv = CreateObject<RttEchoServer>();
        rttSrv->Setup(rttPort);
        serverNode->AddApplication(rttSrv);
        rttSrv->SetStartTime(Seconds(0.5));
        rttSrv->SetStopTime(Seconds(simTime));

        rttCli = CreateObject<RttCsvProbe>();
        rttCli->Setup(serverIp, rttPort, Seconds(1.0 / rttHz), rttRunPath);
        staNode->AddApplication(rttCli);
        rttCli->SetStartTime(Seconds(std::max(1.0, appStart)));
        rttCli->SetStopTime(Seconds(simTime));
    }

    // ---- routing controller ----
    Ptr<RoamRoutingController> rc = CreateObject<RoamRoutingController>();
    rc->Setup(staNode,
              serverNode,
              ap1Node,
              ap2Node,
              staDev.Get(0),
              csmaDevs.Get(2),
              staIp,
              ap1WifiIp,
              ap2WifiIp,
              ap1CsmaIp,
              ap2CsmaIp,
              ap1Bssid,
              ap2Bssid);

    // ---- loggers ----
    // FIX: start logging after association stabilizes and after movement begins
    const double roamLogStart = std::max(appStart, moveStart) + 0.5;

    RoamingLogger roamGlobal;
    roamGlobal.Init(staMacBase, static_cast<uint8_t>(linkId), roamPath, roamPollS, false); // append=false
    roamGlobal.SetRoutingController(rc);
    roamGlobal.StartAt(roamLogStart);

    RoamingLogger roamRun;
    roamRun.Init(staMacBase, static_cast<uint8_t>(linkId), roamRunPath, roamPollS, false);
    roamRun.SetRoutingController(rc);
    roamRun.StartAt(roamLogStart);

    ThroughputSampler thrGlobal;
    thrGlobal.Init(sink, thrPath, interval, false); // append=false (global is already truncated)
    ThroughputSampler thrRun;
    thrRun.Init(sink, thrRunPath, interval, false);

    thrGlobal.StartAt(std::max(appStart, 0.001));
    thrRun.StartAt(std::max(appStart, 0.001));

    PositionLogger pos;
    pos.Init(staMobModel, posRunPath, posPollS);
    pos.StartAt(0.001);

    // ---- PCAP ----
    if (pcap)
    {
        std::ostringstream pfx;
        pfx << outDir << "/raw/p6_" << tag.str();
        phyAp1.EnablePcap(pfx.str() + "_ap1", ap1Dev.Get(0), true);
        phyAp2.EnablePcap(pfx.str() + "_ap2", ap2Dev.Get(0), true);
        phySta.EnablePcap(pfx.str() + "_sta", staDev.Get(0), true);
        csma.EnablePcap(pfx.str() + "_csma", csmaDevs.Get(0), true);
    }

    // ---- FlowMonitor ----
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> monitor;
    if (flowmon)
        monitor = fmHelper.InstallAll();

    // ---- run ----
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ---- stop loggers ----
    thrGlobal.Stop();
    thrRun.Stop();
    pos.Stop();
    roamGlobal.Stop();
    roamRun.Stop();

    // ---- flowmon output ----
    if (flowmon && monitor)
    {
        monitor->CheckForLostPackets();
        std::ostringstream fmp;
        fmp << outDir << "/raw/flowmon_" << tag.str() << ".xml";
        monitor->SerializeToXmlFile(fmp.str(), true, true);
    }

    // ---- compute summary ----
    const uint64_t rxBytes = sink ? sink->GetTotalRx() : 0;
    const double useful = simTime - appStart;
    const double goodputBps = (useful > 0.0) ? (8.0 * static_cast<double>(rxBytes) / useful) : 0.0;

    // pick first real roam time from per-run file
    const double roamTime = roamRun.GetFirstRoamTime();

    double rttMeanMs = 0.0;
    uint32_t rttSamples = 0;

    if (enableRttProbe && IsFileNonEmpty(rttRunPath))
    {
        std::ifstream in(rttRunPath);
        std::string line;
        std::getline(in, line); // header
        double rttSum = 0.0;

        while (std::getline(in, line))
        {
            if (line.empty())
                continue;
            std::istringstream ss(line);
            std::string t, seq, r;
            std::getline(ss, t, ',');
            std::getline(ss, seq, ',');
            std::getline(ss, r, ',');
            if (r.empty())
                continue;

            try
            {
                double v = std::stod(r);
                if (std::isfinite(v) && v >= 0.0)
                {
                    rttSum += v;
                    ++rttSamples;
                }
            }
            catch (...)
            {
            }
        }

        if (rttSamples > 0)
            rttMeanMs = rttSum / static_cast<double>(rttSamples);

        // generate ping-like txt
        {
            std::ifstream in2(rttRunPath);
            std::ofstream out(pingTxtPath, std::ios::out | std::ios::trunc);
            std::string hdr;
            std::getline(in2, hdr);
            out << "time_s rtt_ms\n";

            std::string row;
            while (std::getline(in2, row))
            {
                if (row.empty())
                    continue;
                std::istringstream ss(row);
                std::string tStr, seqStr, rStr;
                std::getline(ss, tStr, ',');
                std::getline(ss, seqStr, ',');
                std::getline(ss, rStr, ',');
                if (tStr.empty() || rStr.empty())
                    continue;

                out << std::fixed << std::setprecision(6)
                    << std::stod(tStr) << " " << std::stod(rStr) << "\n";
            }
        }
    }

    // ---- summary CSV ----
    const bool needHeader = !IsFileNonEmpty(sumPath);
    std::ofstream sumFile(sumPath, std::ios::out | std::ios::app);
    if (needHeader)
        sumFile << "apDistance,staSpeed,moveStart,udpRate,pktSize,seed,run,rxBytes,goodputbps,roamTime\n";
    sumFile << std::fixed << std::setprecision(6)
            << apDistance << ","
            << staSpeed << ","
            << moveStart << ","
            << udpRate << ","
            << pktSize << ","
            << seed << ","
            << run << ","
            << rxBytes << ","
            << goodputBps << ","
            << roamTime << "\n";
    sumFile.close();

    std::cout << "[P6] run=" << run
              << " speed=" << staSpeed
              << " goodput(Mbps)=" << (goodputBps / 1e6)
              << " roamTime(s)=" << roamTime
              << " rttMean(ms)=" << rttMeanMs
              << " samples=" << rttSamples
              << std::endl;

    Simulator::Destroy();
    return 0;
}
