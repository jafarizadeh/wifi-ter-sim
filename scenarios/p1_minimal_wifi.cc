// p1_minimal_wifi.cc (part 1 - ns-3)

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/ping-helper.h"   // PingHelper (ns-3.46+)

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace ns3;

int main(int argc, char* argv[])
{
  // Paramètres obligatoires (CommandLine)
  std::string ssidStr = "wifi-demo";
  double simTime = 10.0;
  double distance = 5.0;
  bool pcap = true;
  std::string outDir = "results/p1";

  CommandLine cmd;
  cmd.AddValue("ssid", "Nom du réseau Wi-Fi (SSID)", ssidStr);
  cmd.AddValue("simTime", "Durée de simulation (s)", simTime);
  cmd.AddValue("distance", "Distance AP-STA (m)", distance);
  cmd.AddValue("pcap", "Activer la capture pcap", pcap);
  cmd.AddValue("outDir", "Répertoire de sortie", outDir);
  cmd.Parse(argc, argv);

  // Répertoires de sortie
  std::filesystem::create_directories(outDir + "/raw");
  std::filesystem::create_directories(outDir + "/logs");
  std::filesystem::create_directories(outDir + "/plots");

  // Topologie: 1 AP + 1 STA
  NodeContainer apNode;
  apNode.Create(1);
  NodeContainer staNode;
  staNode.Create(1);

  // Mobilité: positions fixes (AP à l'origine, STA à 'distance')
  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
  mobility.Install(apNode);
  mobility.Install(staNode);

  apNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(0.0, 0.0, 0.0));
  staNode.Get(0)->GetObject<MobilityModel>()->SetPosition(Vector(distance, 0.0, 0.0));

  // Wi-Fi (Yans): canal + PHY
  YansWifiChannelHelper channel = YansWifiChannelHelper::Default();
  YansWifiPhyHelper phy;
  phy.SetChannel(channel.Create());

  // Wi-Fi: standard + MAC (SSID identique sur AP et STA)
  WifiHelper wifi;
  wifi.SetStandard(WIFI_STANDARD_80211ax);

  WifiMacHelper mac;
  Ssid ssid = Ssid(ssidStr);

  mac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));
  NetDeviceContainer apDev = wifi.Install(phy, mac, apNode);

  mac.SetType("ns3::StaWifiMac",
              "Ssid", SsidValue(ssid),
              "ActiveProbing", BooleanValue(false));
  NetDeviceContainer staDev = wifi.Install(phy, mac, staNode);

  // Pile IP + IPv4
  InternetStackHelper internet;
  internet.Install(apNode);
  internet.Install(staNode);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.0.0", "255.255.255.0");

  Ipv4InterfaceContainer apIf = ipv4.Assign(apDev);
  Ipv4InterfaceContainer staIf = ipv4.Assign(staDev);

  Ipv4Address apIp = apIf.GetAddress(0);

  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  // Ping: STA -> AP (démarre à 1s)
  PingHelper ping(apIp);
  ping.SetAttribute("VerboseMode", EnumValue(Ping::VERBOSE));
  ApplicationContainer pingApps = ping.Install(staNode.Get(0));
  pingApps.Start(Seconds(1.0));
  pingApps.Stop(Seconds(simTime));

  // PCAP (dans outDir/raw)
  if (pcap)
  {
    std::ostringstream base;
    base << outDir << "/raw/"
         << "wifi_" << ssidStr << "_d" << static_cast<int>(distance) << "m";
    phy.EnablePcap(base.str() + "_ap", apDev.Get(0), true);
    phy.EnablePcap(base.str() + "_sta", staDev.Get(0), true);
  }

  Simulator::Stop(Seconds(simTime));
  Simulator::Run();
  Simulator::Destroy();

  // Petit résumé (utile pour le rapport)
  std::ofstream f(outDir + "/logs/summary.txt");
  if (f)
  {
    f << "part1 summary\n";
    f << "SSID=" << ssidStr << "\n";
    f << "simTime=" << simTime << "\n";
    f << "distance=" << distance << "\n";
    f << "AP_IP=" << apIp << "\n";
    f << "pcap=" << (pcap ? "true" : "false") << "\n";
  }

  std::cout << "part1 OK - pcap dans: " << outDir << "/raw/\n";
  return 0;
}
