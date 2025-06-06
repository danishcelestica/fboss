// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>
#include "fboss/agent/AgentConfig.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwTestEcmpUtils.h"
#include "fboss/agent/test/link_tests/LinkTest.h"
#include "fboss/agent/test/link_tests/LinkTestUtils.h"
#include "fboss/agent/test/utils/PortTestUtils.h"
#include "fboss/lib/CommonFileUtils.h"
#include "fboss/lib/CommonUtils.h"
#include "fboss/lib/thrift_service_client/ThriftServiceClient.h"

using namespace ::testing;
using namespace facebook::fboss;

struct SpeedAndProfile {
  cfg::PortSpeed speed;
  cfg::PortProfileID profileID;
  SpeedAndProfile(cfg::PortSpeed _speed, cfg::PortProfileID _profileID)
      : speed(_speed), profileID(_profileID) {}
};

class SpeedChangeTest : public LinkTest {
 private:
  std::vector<link_test_production_features::LinkTestProductionFeature>
  getProductionFeatures() const override {
    return {
        link_test_production_features::LinkTestProductionFeature::L2_LINK_TEST};
  }

 public:
  void TearDown() override;
  void setupConfigFlag() override;

 protected:
  // Query Qsfp service for eligible port profiles. If current port speed is the
  // same as from_speed and there exists eligible port profiles for to_speed,
  // return that info to the caller.
  std::map<int, cfg::PortProfileID> getEligibleOpticalPortsAndProfile(
      cfg::SwitchConfig cfg,
      cfg::PortSpeed fromSpeed,
      cfg::PortSpeed toSpeed) {
    std::map<std::string, std::vector<cfg::PortProfileID>> supportedProfiles;
    auto qsfpServiceClient = utils::createQsfpServiceClient();
    qsfpServiceClient->sync_getAllPortSupportedProfiles(
        supportedProfiles, true /* checkOptics */);

    std::vector<int32_t> transceiverIds;
    // Get all transceiverIDs for the ports
    for (const auto& port : *cfg.ports()) {
      PortID portID = PortID(*port.logicalID());
      auto tcvrId =
          platform()->getPlatformPort(portID)->getTransceiverID().value();
      transceiverIds.push_back(tcvrId);
    }

    auto transceiverInfos = utility::waitForTransceiverInfo(transceiverIds);
    std::map<int, cfg::PortProfileID> eligiblePortsAndProfile;
    for (const auto& port : *cfg.ports()) {
      CHECK(port.name().has_value());
      auto tcvrId = platform()
                        ->getPlatformPort(PortID(*port.logicalID()))
                        ->getTransceiverID()
                        .value();
      auto tcvrInfo = transceiverInfos.find(tcvrId);
      if (tcvrInfo != transceiverInfos.end()) {
        auto tcvrState = *tcvrInfo->second.tcvrState();
        if (TransmitterTechnology::OPTICAL ==
            tcvrState.cable().value_or({}).transmitterTech()) {
          // Only consider optical ports
          if (*port.speed() == fromSpeed &&
              supportedProfiles.find(*port.name()) != supportedProfiles.end()) {
            for (const auto& profile : supportedProfiles.at(*port.name())) {
              if (utility::getSpeed(profile) == toSpeed) {
                eligiblePortsAndProfile.emplace(*port.logicalID(), profile);
                break;
              }
            }
          }
        }
      }
    }
    return eligiblePortsAndProfile;
  }

  cfg::SwitchConfig createSpeedChangeConfig(
      cfg::PortSpeed fromSpeed,
      cfg::PortSpeed toSpeed) {
    cfg::AgentConfig testConfig = platform()->config()->thrift;
    auto swConfig = *testConfig.sw();

    // Iterate through ports to find eligible ports
    auto eligilePortsAndProfile =
        getEligibleOpticalPortsAndProfile(swConfig, fromSpeed, toSpeed);
    CHECK_GT(eligilePortsAndProfile.size(), 0);

    for (auto& port : *swConfig.ports()) {
      auto iter = eligilePortsAndProfile.find(*port.logicalID());
      if (iter != eligilePortsAndProfile.end()) {
        auto desiredProfileId = iter->second;
        XLOG(INFO) << folly::sformat(
            "Changing speed and profile on port {:s} from speed={:s},profile={:s} to speed={:s},profile={:s}",
            port.name().ensure(),
            apache::thrift::util::enumName(*port.speed()),
            apache::thrift::util::enumName(*port.profileID()),
            apache::thrift::util::enumName(utility::getSpeed(desiredProfileId)),
            apache::thrift::util::enumName(desiredProfileId));
        PortID portID = PortID(*port.logicalID());
        utility::configurePortProfile(
            platform()->getPlatformMapping(),
            platform()->supportsAddRemovePort(),
            swConfig,
            desiredProfileId,
            utility::getAllPortsInGroup(sw()->getPlatformMapping(), portID),
            portID);
      }
    }

    return swConfig;
  }

  void runSpeedChangeTest(cfg::PortSpeed fromSpeed, cfg::PortSpeed toSpeed) {
    auto setup = [this, fromSpeed, toSpeed]() {
      auto newConfig = createSpeedChangeConfig(fromSpeed, toSpeed);

      std::atomic<bool> speedChangeDone{false};

      std::thread t([&speedChangeDone]() {
        std::unique_ptr<apache::thrift::Client<facebook::fboss::FbossCtrl>>
            agentClient = nullptr;
        std::map<int32_t, facebook::fboss::PortInfoThrift> portEntries;

        try {
          agentClient = utils::createWedgeAgentClient();
        } catch (const std::exception& ex) {
          XLOG(WARN) << "Failed createWedgeAgentClient call. "
                     << folly::exceptionStr(ex);
        }

        if (agentClient != nullptr) {
          while (!speedChangeDone) {
            try {
              agentClient->sync_getAllPortInfo(portEntries);
            } catch (const std::exception& ex) {
              XLOG(WARN) << "Failed getAllPortInfo() thrift call. "
                         << folly::exceptionStr(ex);
            }
          }
        }
      });

      // Apply the new config
      sw()->applyConfig("set secondary speeds", newConfig);

      EXPECT_NO_THROW(waitForAllCabledPorts(true));
      createL3DataplaneFlood();
      speedChangeDone = true;
      t.join();
    };
    auto verify = [this]() {
      /*
       * Verify the following on all cabled ports
       * 1. Link comes up at secondary speeds
       * 2. LLDP neighbor is discovered
       * 3. Assert no in discards
       */
      EXPECT_NO_THROW(waitForAllCabledPorts(true));
      checkWithRetry(
          [this]() { return sendAndCheckReachabilityOnAllCabledPorts(); });
      // Assert no traffic loss and no ecmp shrink. If ports flap
      // these conditions will not be true
      assertNoInDiscards();

      if (platform()->getHwSwitch()->getBootType() == BootType::COLD_BOOT) {
        auto ecmpSizeInSw = getSingleVlanOrRoutedCabledPorts().size();
        XLOG(INFO) << "[DBG] ECMP size in sw: " << ecmpSizeInSw;
        WITH_RETRIES({
          EXPECT_EVENTUALLY_EQ(
              utility::getEcmpSizeInHw(
                  platform()->getHwSwitch(),
                  {folly::IPAddress("::"), 0},
                  RouterID(0),
                  ecmpSizeInSw),
              ecmpSizeInSw);
        });
      }
    };
    verifyAcrossWarmBoots(setup, verify);
  }

  std::string originalConfigCopy;
};

void SpeedChangeTest::setupConfigFlag() {
  // Coldboot iteration -
  //    SetUp -
  //      1. Make a copy of the original config. Existing copy is overridden
  //      2. The speeds and profiles are modified by this test in the original
  //      config.
  //    TearDown - Do nothing. Keep the modified config in the
  //    FLAGS_config path.
  // Warmboot iteration -
  //    SetUp - Do nothing. Agent loads up with the modified config (setup by
  //      coldboot iteration)
  //    TearDown - Copy is deleted if we are not asked to setup_for_warmboot
  //    again
  auto linkTestDir = FLAGS_volatile_state_dir + "/link_test";
  originalConfigCopy =
      linkTestDir + "/agent_speed_change_test_original_config.conf";

  // Create a copy of the original config if we haven't created one before.
  if (!boost::filesystem::exists(originalConfigCopy)) {
    // Likely a coldboot iteration
    utilCreateDir(linkTestDir);
    boost::filesystem::copy_file(FLAGS_config, originalConfigCopy);
  }
  // Make sure the copy exists
  CHECK(boost::filesystem::exists(originalConfigCopy));
}

void SpeedChangeTest::TearDown() {
  if (!FLAGS_list_production_feature) {
    if (!FLAGS_setup_for_warmboot) {
      // If this test wasn't setup for warmboot, revert back the original config
      // in FLAGS_config path. Also clean up the copy
      boost::filesystem::copy_file(
          originalConfigCopy,
          FLAGS_config,
          boost::filesystem::copy_option::overwrite_if_exists);
      CHECK(removeFile(originalConfigCopy));
    }
  }
  LinkTest::TearDown();
}

#define TEST_SPEED_CHANGE(from_speed, to_speed)                               \
  TEST_F(SpeedChangeTest, from_speed##To##to_speed) {                         \
    runSpeedChangeTest(cfg::PortSpeed::from_speed, cfg::PortSpeed::to_speed); \
  }

TEST_SPEED_CHANGE(TWOHUNDREDG, HUNDREDG);

TEST_SPEED_CHANGE(HUNDREDG, TWOHUNDREDG);

TEST_SPEED_CHANGE(TWOHUNDREDG, FOURHUNDREDG);

TEST_SPEED_CHANGE(FOURHUNDREDG, TWOHUNDREDG);
