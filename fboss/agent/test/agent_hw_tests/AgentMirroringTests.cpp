// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/AgentHwTest.h"

#include "fboss/agent/AsicUtils.h"
#include "fboss/agent/packet/PktFactory.h"
#include "fboss/agent/packet/PktUtil.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/utils/AclTestUtils.h"
#include "fboss/agent/test/utils/ConfigUtils.h"
#include "fboss/agent/test/utils/MirrorTestUtils.h"
#include "fboss/agent/test/utils/PacketSnooper.h"
#include "fboss/lib/CommonUtils.h"

#include <folly/IPAddress.h>
#include "fboss/agent/TxPacket.h"

#include "fboss/agent/test/gen-cpp2/production_features_types.h"

#include <boost/range/combine.hpp>
#include <folly/logging/xlog.h>
#include "fboss/lib/config/PlatformConfigUtils.h"

namespace {
using TestTypes = ::testing::Types<folly::IPAddressV4, folly::IPAddressV6>;
const std::string kIngressSpan = "ingress_span";
const std::string kIngressErspan = "ingress_erspan";
const std::string kEgressSpan = "egress_span";
const std::string kEgressErspan = "egress_erspan";

const std::string kMirrorAcl = "mirror_acl";

} // namespace

namespace facebook::fboss {

template <typename AddrT>
class AgentMirroringTest : public AgentHwTest {
 public:
  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto config = utility::onePortPerInterfaceConfig(
        ensemble.getSw(),
        ensemble.masterLogicalPortIds(),
        true /*interfaceHasSubnet*/);
    return config;
  }

  PortID getMirrorToPort(const AgentEnsemble& ensemble) const {
    return ensemble.masterLogicalPortIds(
        {cfg::PortType::INTERFACE_PORT})[utility::kMirrorToPortIndex];
  }

  PortID getTrafficPort(const AgentEnsemble& ensemble) const {
    return ensemble.masterLogicalPortIds(
        {cfg::PortType::INTERFACE_PORT})[utility::kTrafficPortIndex];
  }

  void addPortMirrorConfig(
      cfg::SwitchConfig* cfg,
      const AgentEnsemble& ensemble,
      const std::string& mirrorName) const {
    auto trafficPort = getTrafficPort(ensemble);
    auto portConfig = utility::findCfgPort(*cfg, trafficPort);
    if (mirrorName == utility::kIngressSpan ||
        mirrorName == utility::kIngressErspan) {
      portConfig->ingressMirror() = mirrorName;
    } else if (
        mirrorName == utility::kEgressSpan ||
        mirrorName == utility::kEgressErspan) {
      portConfig->egressMirror() = mirrorName;
    } else {
      throw FbossError("Invalid mirror name ", mirrorName);
    }
  }

  void sendPackets(int count, size_t payloadSize = 1) {
    auto params = utility::getMirrorTestParams<AddrT>();
    auto vlanId = getVlanIDForTx();
    auto intfMac =
        utility::getMacForFirstInterfaceWithPorts(getProgrammedState());
    std::vector<uint8_t> payload(payloadSize, 0xff);
    auto trafficPort = getTrafficPort(*getAgentEnsemble());
    auto oldPacketStats = getLatestPortStats(trafficPort);
    auto oldOutPkts = *oldPacketStats.outUnicastPkts_();
    auto i = 0;
    while (i < count) {
      auto pkt = utility::makeUDPTxPacket(
          getSw(),
          vlanId,
          intfMac,
          intfMac,
          params.senderIp,
          params.receiverIp,
          srcL4Port_,
          dstL4Port_,
          0,
          255,
          payload);
      getSw()->sendPacketSwitchedAsync(std::move(pkt));
      i++;
    }
    WITH_RETRIES({
      auto portStats = getLatestPortStats(trafficPort);
      auto newOutPkt = *portStats.outUnicastPkts_() +
          *portStats.outMulticastPkts_() + *portStats.outBroadcastPkts_();
      EXPECT_EVENTUALLY_GE(newOutPkt, oldOutPkts);
      EXPECT_EVENTUALLY_EQ(newOutPkt - oldOutPkts, count);
    });
  }

  RoutePrefix<AddrT> getMirrorRoutePrefix(const folly::IPAddress dip) const {
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      return RoutePrefix<AddrT>{
          folly::IPAddressV4{dip.str()}, static_cast<uint8_t>(dip.bitCount())};
    } else {
      return RoutePrefix<AddrT>{
          folly::IPAddressV6{dip.str()}, static_cast<uint8_t>(dip.bitCount())};
    }
  }

  template <typename T = AddrT>
  void resolveMirror(const std::string& mirrorName) {
    utility::EcmpSetupAnyNPorts<AddrT> ecmpHelper(
        getProgrammedState(), getSw()->needL2EntryForNeighbor());
    auto trafficPort = getTrafficPort(*getAgentEnsemble());
    auto mirrorToPort = getMirrorToPort(*getAgentEnsemble());
    EXPECT_EQ(trafficPort, ecmpHelper.nhop(0).portDesc.phyPortID());
    EXPECT_EQ(mirrorToPort, ecmpHelper.nhop(1).portDesc.phyPortID());
    resolveNeighborAndProgramRoutes(ecmpHelper, 1);
    applyNewState([&](const std::shared_ptr<SwitchState>& in) {
      boost::container::flat_set<PortDescriptor> nhopPorts{
          PortDescriptor(mirrorToPort)};
      return utility::EcmpSetupAnyNPorts<AddrT>(
                 in, getSw()->needL2EntryForNeighbor())
          .resolveNextHops(in, nhopPorts);
    });
    getSw()->getUpdateEvb()->runInFbossEventBaseThreadAndWait([] {});
    auto mirror = getSw()->getState()->getMirrors()->getNodeIf(mirrorName);
    auto dip = mirror->getDestinationIp();
    if (dip.has_value()) {
      auto prefix = getMirrorRoutePrefix(dip.value());
      boost::container::flat_set<PortDescriptor> nhopPorts{
          PortDescriptor(mirrorToPort)};
      auto wrapper = getSw()->getRouteUpdater();
      ecmpHelper.programRoutes(&wrapper, nhopPorts, {prefix});
    }
  }

  void addAclMirrorConfig(
      cfg::SwitchConfig* cfg,
      const AgentEnsemble& ensemble,
      const std::string& mirrorName) const {
    auto trafficPort = getTrafficPort(ensemble);
    std::string aclEntryName = kMirrorAcl;
    auto aclEntry = cfg::AclEntry();
    aclEntry.name() = aclEntryName;
    aclEntry.actionType() = cfg::AclActionType::PERMIT;
    aclEntry.l4SrcPort() = srcL4Port_;
    aclEntry.l4DstPort() = dstL4Port_;
    aclEntry.dstPort() = trafficPort;
    aclEntry.proto() = 17;
    /*
     * The number of packets mirrorred through ACL is different in Native BCM
     * and BCM-SAI (CS00012203195). The number of packets mirrorred in Native
     * BCM is 2 whereas number of packets mirrored in BCM-SAI is 4.
     *
     * 1) Send packet from CPU and perform hw lookup in the ASIC
     * 2) Packet is routed and forwarded to Port 0
     * 3) Packet is mirrored (ingress and egress) to port 1
     * 4) For native BCM, the packet routed out of port 0 is looped back
     *    and dropped.
     *    For BCM-SAI, the packet routed out of port 0 is looped back
     *    but L2 forwarded, ACL is looked up which mirrors again (2
     *    more packets) and dropped due to same port check.
     *
     *  The reason for the difference is the packet in native BCM is
     *  treated as unknown unicast and the dst port lookup is not performed
     *  in IFP for unknown unicast packets. Whereas in BCM-SAI, we
     *  install a FDB entry for every neighbor entry. The FDB entry is hit
     *  which makes the packet to be treated as known unicast and the IFP
     *  is hit and the packets are mirrored again.
     *
     * In order to avoid the ACL lookup, we can add additional qualifiers.
     * - SRC PORT: can be used once TAJO supports it.
     * - DMAC: DMAC can be used since the routed packet will have a different
     *   DMAC. Not all platforms support DMAC as a qualifier (For eg, Tajo)
     * - DROP Bit: WB + Fitting implications
     * - TTL: Use TTL 255 to ensure this entry is hit only for the first packet.
     *
     * For now, using TTL qualifier to ensure the routed packet is dropped. We
     * can update the test to include source port qualifier with CPU port
     * and enable it on TAJO platform.
     */
    cfg::Ttl ttl;
    ttl.value() = 255;
    ttl.mask() = 0xFF;
    aclEntry.ttl() = ttl;
    utility::addAclEntry(cfg, aclEntry, utility::kDefaultAclTable());

    cfg::MatchAction matchAction = cfg::MatchAction();
    if (mirrorName == utility::kIngressErspan) {
      matchAction.ingressMirror() = mirrorName;
    } else {
      matchAction.egressMirror() = mirrorName;
    }
    cfg::MatchToAction matchToAction = cfg::MatchToAction();
    matchToAction.matcher() = aclEntryName;
    matchToAction.action() = matchAction;
    cfg->dataPlaneTrafficPolicy() = cfg::TrafficPolicyConfig();
    cfg->dataPlaneTrafficPolicy()->matchToAction()->push_back(matchToAction);
  }

  void verifyMirrorProgrammed(
      apache::thrift::Client<utility::AgentHwTestCtrl>* client,
      const state::MirrorFields& fields) {
    WITH_RETRIES(
        { EXPECT_EVENTUALLY_TRUE(client->sync_isMirrorProgrammed(fields)); });
  }

  void verifyPortMirrorProgrammed(const std::string& mirrorName) {
    auto mirror = getProgrammedState()->getMirrors()->getNodeIf(mirrorName);
    EXPECT_NE(mirror, nullptr);
    auto fields = mirror->toThrift();

    auto scope = getAgentEnsemble()->scopeResolver().scope(mirror);
    for (auto switchID : scope.switchIds()) {
      auto client = getAgentEnsemble()->getHwAgentTestClient(switchID);
      verifyMirrorProgrammed(client.get(), fields);
      WITH_RETRIES({
        EXPECT_EVENTUALLY_TRUE(client->sync_isPortMirrored(
            getTrafficPort(*getAgentEnsemble()), mirrorName, isIngress()));
      });
    }
  }

  void verifyAclMirrorProgrammed(const std::string& mirrorName) {
    auto mirror = getProgrammedState()->getMirrors()->getNodeIf(mirrorName);
    EXPECT_NE(mirror, nullptr);
    auto fields = mirror->toThrift();

    auto scope = getAgentEnsemble()->scopeResolver().scope(mirror);
    for (auto switchID : scope.switchIds()) {
      auto client = getAgentEnsemble()->getHwAgentTestClient(switchID);
      verifyMirrorProgrammed(client.get(), fields);
      WITH_RETRIES({
        EXPECT_EVENTUALLY_TRUE(client->sync_isAclEntryMirrored(
            kMirrorAcl, mirrorName, isIngress()));
      });
    }
  }

  void verify(const std::string& mirrorName, int payloadSize = 500) {
    auto trafficPort = getTrafficPort(*getAgentEnsemble());
    auto mirrorToPort = getMirrorToPort(*getAgentEnsemble());
    WITH_RETRIES({
      auto ingressMirror =
          this->getProgrammedState()->getMirrors()->getNodeIf(mirrorName);
      ASSERT_NE(ingressMirror, nullptr);
      EXPECT_EVENTUALLY_EQ(ingressMirror->isResolved(), true);
    });
    auto trafficPortPktStatsBefore = getLatestPortStats(trafficPort);
    auto mirrorPortPktStatsBefore = getLatestPortStats(mirrorToPort);

    auto trafficPortPktsBefore = *trafficPortPktStatsBefore.outUnicastPkts_();
    auto mirroredPortPktsBefore = *trafficPortPktStatsBefore.outUnicastPkts_();

    this->sendPackets(1, payloadSize);

    WITH_RETRIES({
      auto trafficPortPktStatsAfter = getLatestPortStats(trafficPort);
      auto trafficPortPktsAfter = *trafficPortPktStatsAfter.outUnicastPkts_();
      EXPECT_EVENTUALLY_EQ(1, trafficPortPktsAfter - trafficPortPktsBefore);
    });

    auto expectedMirrorPackets = 1;
    WITH_RETRIES({
      auto mirrorPortPktStatsAfter = getLatestPortStats(mirrorToPort);
      auto mirroredPortPktsAfter = *mirrorPortPktStatsAfter.outUnicastPkts_();
      EXPECT_EVENTUALLY_EQ(
          mirroredPortPktsAfter - mirroredPortPktsBefore,
          expectedMirrorPackets);
    });
  }

  void testPortMirror(const std::string& mirrorName) {
    auto setup = [=, this]() { this->resolveMirror(mirrorName); };
    auto verify = [=, this]() {
      this->verify(mirrorName);
      this->verifyPortMirrorProgrammed(mirrorName);
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  void testUpdatePortMirror(const std::string& mirrorName) {
    auto setup = [=, this]() {
      this->resolveMirror(mirrorName);
      auto cfg = initialConfig(*getAgentEnsemble());
      cfg.mirrors()->clear();
      utility::addMirrorConfig<AddrT>(
          &cfg,
          *getAgentEnsemble(),
          mirrorName,
          false /* truncate */,
          48 /* dscp */);
      this->applyNewConfig(cfg);
    };
    auto verify = [=, this]() {
      this->verify(mirrorName);
      this->verifyPortMirrorProgrammed(mirrorName);
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  void testAclMirror(const std::string& mirrorName) {
    auto setup = [=, this]() { this->resolveMirror(mirrorName); };
    auto verify = [=, this]() {
      this->verify(mirrorName);
      this->verifyAclMirrorProgrammed(mirrorName);
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  void testUpdateAclMirror(const std::string& mirrorName) {
    auto setup = [=, this]() {
      this->resolveMirror(mirrorName);
      auto cfg = initialConfig(*getAgentEnsemble());
      cfg.mirrors()->clear();
      utility::addMirrorConfig<AddrT>(
          &cfg,
          *getAgentEnsemble(),
          mirrorName,
          false /* truncate */,
          48 /* dscp */);
      this->applyNewConfig(cfg);
    };
    auto verify = [=, this]() {
      this->verify(mirrorName);
      this->verifyAclMirrorProgrammed(mirrorName);
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  void testRemoveMirror(const std::string& mirrorName) {
    auto setup = [=, this]() {
      const auto& ensemble = *getAgentEnsemble();
      this->resolveMirror(mirrorName);

      auto config = utility::onePortPerInterfaceConfig(
          ensemble.getSw(),
          ensemble.masterLogicalPortIds(),
          true /*interfaceHasSubnet*/);

      this->applyNewConfig(config);
    };
    auto verify = [=, this]() {
      auto mirror = getProgrammedState()->getMirrors()->getNodeIf(mirrorName);
      EXPECT_EQ(mirror, nullptr);
      auto scopeResolver = getAgentEnsemble()->getSw()->getScopeResolver();
      auto scope = scopeResolver->scope(mirror);
      for (auto switchID : scope.switchIds()) {
        auto client = getAgentEnsemble()->getHwAgentTestClient(switchID);
        WITH_RETRIES({
          EXPECT_EVENTUALLY_FALSE(client->sync_isPortMirrored(
              getTrafficPort(*getAgentEnsemble()), mirrorName, isIngress()));
          EXPECT_TRUE(client->sync_isAclEntryMirrored(
              kMirrorAcl, mirrorName, isIngress()));
        });
      }
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  void testPortMirrorWithLargePacket(const std::string& mirrorName) {
    auto setup = [=, this]() { this->resolveMirror(mirrorName); };
    auto verify = [=, this]() {
      auto mirrorToPort = getMirrorToPort(*getAgentEnsemble());
      auto statsBefore = getLatestPortStats(mirrorToPort);
      this->verify(mirrorName, 8000);
      WITH_RETRIES({
        auto statsAfter = getLatestPortStats(mirrorToPort);

        auto outBytes = (*statsAfter.outBytes_() - *statsBefore.outBytes_());
        // TODO: on TH3 for v6 packets, 254 bytes are mirrored which is a single
        // MMU cell. but for v4 packets, 234 bytes are mirrored. need to
        // investigate this behavior.
        EXPECT_EVENTUALLY_LE(
            outBytes, 1500 /* payload is truncated to ethernet MTU of 1500 */);
      });
    };
    this->verifyAcrossWarmBoots(setup, verify);
  }

  template <typename T = AddrT>
  bool skipTest() const {
    return std::is_same<T, folly::IPAddressV6>::value &&
        !isSupportedOnAllAsics(HwAsic::Feature::ERSPANv6);
  }

  virtual bool isIngress() const = 0;
  const RouterID kRid{0};
  const uint16_t srcL4Port_{1234};
  const uint16_t dstL4Port_{4321};
};

template <typename AddrT>
class AgentIngressPortSpanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::INGRESS_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressSpan, false /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kIngressSpan);
    return cfg;
  }

  bool isIngress() const override {
    return true;
  }
};

template <typename AddrT>
class AgentIngressPortErspanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::INGRESS_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressErspan, false /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kIngressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return true;
  }
};

template <typename AddrT>
class AgentIngressPortErspanMirroringTruncateTest
    : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::INGRESS_MIRRORING,
        ProductionFeature::MIRROR_PACKET_TRUNCATION};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressErspan, true /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kIngressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return true;
  }
};

template <typename AddrT>
class AgentIngressAclSpanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::INGRESS_MIRRORING,
        ProductionFeature::INGRESS_ACL_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressSpan, false /* truncate */);
    this->addAclMirrorConfig(&cfg, ensemble, utility::kIngressSpan);
    return cfg;
  }

  bool isIngress() const override {
    return true;
  }
};

template <typename AddrT>
class AgentIngressAclErspanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::INGRESS_MIRRORING,
        ProductionFeature::INGRESS_ACL_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressErspan, false /* truncate */);
    this->addAclMirrorConfig(&cfg, ensemble, utility::kIngressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return true;
  }
};

template <typename AddrT>
class AgentEgressPortSpanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::EGRESS_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kEgressSpan, false /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kEgressSpan);
    return cfg;
  }

  bool isIngress() const override {
    return false;
  }
};

template <typename AddrT>
class AgentEgressPortErspanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {ProductionFeature::EGRESS_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kEgressErspan, false /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kEgressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return false;
  }
};

template <typename AddrT>
class AgentEgressPortErspanMirroringTruncateTest
    : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::MIRROR_PACKET_TRUNCATION,
        ProductionFeature::EGRESS_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kEgressErspan, true /* truncate */);
    this->addPortMirrorConfig(&cfg, ensemble, utility::kEgressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return false;
  }
};

template <typename AddrT>
class AgentEgressAclSpanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::EGRESS_MIRRORING,
        ProductionFeature::EGRESS_ACL_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kEgressSpan, false /* truncate */);
    this->addAclMirrorConfig(&cfg, ensemble, utility::kEgressSpan);
    return cfg;
  }

  bool isIngress() const override {
    return false;
  }
};

template <typename AddrT>
class AgentEgressAclErspanMirroringTest : public AgentMirroringTest<AddrT> {
 public:
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    return {
        ProductionFeature::EGRESS_MIRRORING,
        ProductionFeature::EGRESS_ACL_MIRRORING};
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kEgressErspan, false /* truncate */);
    this->addAclMirrorConfig(&cfg, ensemble, utility::kEgressErspan);
    return cfg;
  }

  bool isIngress() const override {
    return false;
  }
};

TYPED_TEST_SUITE(AgentIngressPortSpanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentIngressPortErspanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentIngressPortErspanMirroringTruncateTest, TestTypes);
TYPED_TEST_SUITE(AgentIngressAclSpanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentIngressAclErspanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentEgressPortSpanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentEgressPortErspanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentEgressPortErspanMirroringTruncateTest, TestTypes);
TYPED_TEST_SUITE(AgentEgressAclSpanMirroringTest, TestTypes);
TYPED_TEST_SUITE(AgentEgressAclErspanMirroringTest, TestTypes);

TYPED_TEST(AgentIngressPortSpanMirroringTest, SpanPortMirror) {
  this->testPortMirror(utility::kIngressSpan);
}

TYPED_TEST(AgentIngressPortSpanMirroringTest, UpdateSpanPortMirror) {
  this->testUpdatePortMirror(kIngressSpan);
}

TYPED_TEST(AgentIngressAclSpanMirroringTest, RemoveSpanMirror) {
  this->testRemoveMirror(utility::kIngressSpan);
}

TYPED_TEST(AgentIngressPortErspanMirroringTest, ErspanPortMirror) {
  this->testPortMirror(utility::kIngressErspan);
}

TYPED_TEST(AgentIngressAclSpanMirroringTest, RemoveErspanMirror) {
  this->testRemoveMirror(utility::kIngressErspan);
}

TYPED_TEST(AgentIngressPortErspanMirroringTest, UpdateErspanPortMirror) {
  this->testUpdatePortMirror(kIngressErspan);
}

TYPED_TEST(AgentIngressAclSpanMirroringTest, SpanAclMirror) {
  this->testAclMirror(utility::kIngressSpan);
}

TYPED_TEST(AgentIngressAclSpanMirroringTest, UpdateSpanAclMirror) {
  this->testUpdateAclMirror(kIngressSpan);
}

TYPED_TEST(AgentIngressAclErspanMirroringTest, ErspanAclMirror) {
  this->testAclMirror(utility::kIngressErspan);
}

TYPED_TEST(AgentIngressAclErspanMirroringTest, UpdateErspanAclMirror) {
  this->testUpdateAclMirror(kIngressErspan);
}

TYPED_TEST(
    AgentIngressPortErspanMirroringTruncateTest,
    TrucatePortErspanMirror) {
  this->testPortMirrorWithLargePacket(utility::kIngressErspan);
}

TYPED_TEST(AgentEgressPortSpanMirroringTest, SpanPortMirror) {
  this->testPortMirror(utility::kEgressSpan);
}

TYPED_TEST(AgentEgressPortErspanMirroringTest, ErspanPortMirror) {
  this->testPortMirror(utility::kEgressErspan);
}

TYPED_TEST(AgentEgressAclSpanMirroringTest, SpanAclMirror) {
  this->testAclMirror(utility::kEgressSpan);
}

TYPED_TEST(AgentEgressAclErspanMirroringTest, ErspanAclMirror) {
  this->testAclMirror(utility::kEgressErspan);
}

TYPED_TEST(
    AgentEgressPortErspanMirroringTruncateTest,
    TrucatePortErspanMirror) {
  this->testPortMirrorWithLargePacket(utility::kEgressErspan);
}

template <class AddrT>
class AgentErspanIngressSamplingTest
    : public AgentIngressPortErspanMirroringTruncateTest<AddrT> {
 public:
  using Base = AgentIngressPortErspanMirroringTruncateTest<AddrT>;
  std::vector<ProductionFeature> getProductionFeaturesVerified()
      const override {
    auto features = Base::getProductionFeaturesVerified();
    if constexpr (std::is_same_v<AddrT, folly::IPAddressV4>) {
      features.push_back(ProductionFeature::ERSPANv4_SAMPLING);
    } else if (std::is_same_v<AddrT, folly::IPAddressV6>) {
      features.push_back(ProductionFeature::ERSPANv6_SAMPLING);
    }
    return features;
  }

  cfg::SwitchConfig initialConfig(
      const AgentEnsemble& ensemble) const override {
    auto cfg = AgentMirroringTest<AddrT>::initialConfig(ensemble);
    utility::addMirrorConfig<AddrT>(
        &cfg, ensemble, utility::kIngressErspan, false /* truncate */);
    return cfg;
  }

  void configureSamping(cfg::SwitchConfig* config, int sampleRate) {
    auto ensemble = this->getAgentEnsemble();
    utility::configureSflowSampling(
        *config,
        utility::kIngressErspan,
        {this->getTrafficPort(*ensemble)},
        sampleRate);
  }

  void configureTrapAcl(cfg::SwitchConfig* config) {
    auto ensemble = this->getAgentEnsemble();
    auto asic = checkSameAndGetAsic(ensemble->getL3Asics());
    if (asic->isSupported(HwAsic::Feature::SAI_ACL_ENTRY_SRC_PORT_QUALIFIER)) {
      utility::configureTrapAcl(
          asic, *config, this->getMirrorToPort(*ensemble));
    } else {
      utility::configureTrapAcl(
          asic, *config, std::is_same_v<AddrT, folly::IPAddressV4>);
    }
  }
  bool isIngress() const override {
    return true;
  }

  bool isV4() const {
    return std::is_same_v<AddrT, folly::IPAddressV4>;
  }
};

TYPED_TEST_SUITE(AgentErspanIngressSamplingTest, TestTypes);

TYPED_TEST(AgentErspanIngressSamplingTest, ErspanIngressSampling) {
  auto kSampleRate = 1000;
  auto setup = [=, this]() {
    auto config = this->initialConfig(*this->getAgentEnsemble());
    this->configureSamping(&config, kSampleRate);
    this->applyNewConfig(config);
    this->resolveMirror(utility::kIngressErspan);
  };
  auto verify = [=, this]() {
    auto agentEnsemble = this->getAgentEnsemble();
    auto trafficPort = this->getTrafficPort(*agentEnsemble);
    auto mirrorToPort = this->getMirrorToPort(*agentEnsemble);
    WITH_RETRIES({
      auto ingressMirror = this->getProgrammedState()->getMirrors()->getNodeIf(
          utility::kIngressErspan);
      ASSERT_NE(ingressMirror, nullptr);
      EXPECT_EVENTUALLY_EQ(ingressMirror->isResolved(), true);
    });
    auto trafficPortPktStatsBefore = this->getLatestPortStats(trafficPort);
    auto mirrorPortPktStatsBefore = this->getLatestPortStats(mirrorToPort);

    auto trafficPortPktsBefore = *trafficPortPktStatsBefore.outUnicastPkts_();
    auto mirroredPortPktsBefore = *mirrorPortPktStatsBefore.outUnicastPkts_();

    this->sendPackets(3 * kSampleRate, 1000);

    WITH_RETRIES({
      auto trafficPortPktStatsAfter = this->getLatestPortStats(trafficPort);
      auto trafficPortPktsAfter = *trafficPortPktStatsAfter.outUnicastPkts_();
      EXPECT_EVENTUALLY_EQ(
          3 * kSampleRate, trafficPortPktsAfter - trafficPortPktsBefore);
    });

    auto expectedMirrorPackets = 2; /* expect at least 2 packets */
    WITH_RETRIES({
      auto mirrorPortPktStatsAfter = this->getLatestPortStats(mirrorToPort);
      auto mirroredPortPktsAfter = *mirrorPortPktStatsAfter.outUnicastPkts_();
      EXPECT_EVENTUALLY_GE(
          mirroredPortPktsAfter - mirroredPortPktsBefore,
          expectedMirrorPackets);
    });
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

TYPED_TEST(AgentErspanIngressSamplingTest, SamplePacketFormat) {
  auto setup = [=, this]() {
    auto config = this->initialConfig(*this->getAgentEnsemble());
    this->configureSamping(&config, 1000);
    this->configureTrapAcl(&config);
    this->applyNewConfig(config);
    this->resolveMirror(utility::kIngressErspan);
  };
  auto verify = [=, this]() {
    auto agentEnsemble = this->getAgentEnsemble();
    auto mirrorToPort = this->getMirrorToPort(*agentEnsemble);
    auto trafficPort = this->getTrafficPort(*agentEnsemble);

    auto platformMapping =
        this->getAgentEnsemble()->getSw()->getPlatformMapping();
    auto platformPortEntry = platformMapping->getPlatformPort(trafficPort);
    auto chips = platformMapping->getChips();
    auto profileID = this->getAgentEnsemble()
                         ->getProgrammedState()
                         ->getPorts()
                         ->getNode(trafficPort)
                         ->getProfileID();
    // Below is CHENAB specific computation
    // ingress label port is calculated based on the lane as follows
    // ingress label port = ((module + 1) << 4 | split)
    // module is first lane / 8 and split is 0 or 1 depending on first lane
    auto lanes = utility::getHwPortLanes(
        platformPortEntry, profileID, chips, [](auto, auto lane) {
          return lane;
        });
    auto module = lanes[0] / 8;
    auto split = lanes[0] % 8;
    auto ingressLabelPort = ((module + 1) << 4 | split);

    WITH_RETRIES({
      auto ingressMirror = this->getProgrammedState()->getMirrors()->getNodeIf(
          utility::kIngressErspan);
      ASSERT_NE(ingressMirror, nullptr);
      EXPECT_EVENTUALLY_EQ(ingressMirror->isResolved(), true);
    });

    WITH_RETRIES({
      utility::SwSwitchPacketSnooper snooper(
          this->getSw(), "snooper", mirrorToPort);
      snooper.ignoreUnclaimedRxPkts();
      this->sendPackets(1000, 1000);
      auto buf = snooper.waitForPacket(1);
      if (buf.has_value()) {
        // Intentionally dumping to develop deep packet inspection
        XLOG(INFO) << PktUtil::hexDump(buf.value().get());
        auto ensemble = this->getAgentEnsemble();
        auto asic = checkSameAndGetAsic(ensemble->getL3Asics());
        if (asic->getAsicType() == cfg::AsicType::ASIC_TYPE_CHENAB) {
          folly::io::Cursor cursor(buf.value().get());
          cursor += 14; // skip ethernet header
          if (this->isV4()) {
            cursor += 20; // skip IPv4 header
          } else {
            cursor += 40; // skip IPv6 header
          }
          auto gre = cursor.readBE<uint32_t>(); // read gre proto
          EXPECT_EQ(gre, 0x8949);
          auto port = cursor.readBE<uint16_t>(); // ingress label port
          EXPECT_EQ(port, ingressLabelPort);
          auto opcode = cursor.readBE<uint8_t>(); // opcode
          EXPECT_EQ(opcode, 0x1); // v1 erspan
          cursor.readBE<uint8_t>(); // padding
          cursor.readBE<uint8_t>(); // flags
          cursor.readBE<uint8_t>(); // packet type
        }
      }
      EXPECT_EVENTUALLY_TRUE(buf.has_value());
    });
  };
  this->verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
