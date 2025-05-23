/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmUdfManager.h"
#include "fboss/agent/hw/bcm/tests/BcmTest.h"
#include "fboss/agent/hw/bcm/tests/BcmTestUtils.h"
#include "fboss/agent/hw/test/LoadBalancerUtils.h"
#include "fboss/agent/packet/IPProto.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/agent/test/utils/UdfTestUtils.h"

#include <memory>

using namespace facebook::fboss;

namespace facebook::fboss {

class BcmUdfTest : public BcmTest {
 protected:
  std::shared_ptr<SwitchState> setupUdfConfiguration(
      bool addConfig,
      bool udfHash = true) {
    auto udfConfigState = std::make_shared<UdfConfig>();
    cfg::UdfConfig udfConfig;
    if (addConfig) {
      if (udfHash) {
        udfConfig = utility::addUdfHashConfig(getAsicType());
      } else {
        udfConfig = utility::addUdfAclConfig();
      }
    }
    udfConfigState->fromThrift(udfConfig);

    auto state = getProgrammedState();
    state->modify(&state);
    auto switchSettings = utility::getFirstNodeIf(state->getSwitchSettings());
    auto newSwitchSettings = switchSettings->modify(&state);
    newSwitchSettings->setUdfConfig(udfConfigState);
    return state;
  }
};

TEST_F(BcmUdfTest, checkUdfHashGroupConfiguration) {
  auto setupUdfConfig = [=]() {
    applyNewState(setupUdfConfiguration(true, true));
  };
  auto verifyUdfConfig = [=]() {
    int udfGroupId;
    udfGroupId = getHwSwitch()->getUdfMgr()->getBcmUdfGroupId(
        utility::kUdfHashDstQueuePairGroupName);

    /* get udf info */
    bcm_udf_t udfInfo;
    bcm_udf_t_init(&udfInfo);
    auto rv = bcm_udf_get(getHwSwitch()->getUnit(), udfGroupId, &udfInfo);
    bcmCheckError(rv, "Unable to get udfInfo for udfGroupId: ", udfGroupId);
    ASSERT_EQ(udfInfo.layer, bcmUdfLayerL4OuterHeader);
    ASSERT_EQ(
        udfInfo.start, utility::kUdfHashDstQueuePairStartOffsetInBytes * 8);
    ASSERT_EQ(udfInfo.width, utility::kUdfHashDstQueuePairFieldSizeInBytes * 8);
  };

  verifyAcrossWarmBoots(setupUdfConfig, verifyUdfConfig);
};

TEST_F(BcmUdfTest, checkUdfAclGroupConfiguration) {
  auto setupUdfConfig = [=]() {
    applyNewState(setupUdfConfiguration(true, false));
  };
  auto verifyUdfConfig = [=]() {
    int udfGroupId;
    udfGroupId = getHwSwitch()->getUdfMgr()->getBcmUdfGroupId(
        utility::kUdfAclRoceOpcodeGroupName);
    XLOG(DBG5) << "checkUdfAclGroupConfiguration udfGroupId: " << udfGroupId;
    /* get udf info */
    bcm_udf_t udfInfo;
    bcm_udf_t_init(&udfInfo);
    auto rv = bcm_udf_get(getHwSwitch()->getUnit(), udfGroupId, &udfInfo);
    bcmCheckError(rv, "Unable to get udfInfo for udfGroupId: ", udfGroupId);
    ASSERT_EQ(udfInfo.layer, bcmUdfLayerL4OuterHeader);
    ASSERT_EQ(udfInfo.start, utility::kUdfAclRoceOpcodeStartOffsetInBytes * 8);
    ASSERT_EQ(udfInfo.width, utility::kUdfAclRoceOpcodeFieldSizeInBytes * 8);
  };

  verifyAcrossWarmBoots(setupUdfConfig, verifyUdfConfig);
};

TEST_F(BcmUdfTest, checkUdfPktMatcherConfiguration) {
  auto setupUdfConfig = [=]() { applyNewState(setupUdfConfiguration(true)); };
  auto verifyUdfConfig = [=]() {
    const int udfPacketMatcherId =
        getHwSwitch()->getUdfMgr()->getBcmUdfPacketMatcherId(
            utility::kUdfL4UdpRocePktMatcherName);
    /* get udf pkt info */
    bcm_udf_pkt_format_info_t pktFormat;
    bcm_udf_pkt_format_info_t_init(&pktFormat);
    auto rv = bcm_udf_pkt_format_info_get(
        getHwSwitch()->getUnit(), udfPacketMatcherId, &pktFormat);
    bcmCheckError(
        rv,
        "Unable to get pkt_format for udfPacketMatcherId: ",
        udfPacketMatcherId);
    ASSERT_EQ(pktFormat.ip_protocol, static_cast<int>(IP_PROTO::IP_PROTO_UDP));
    ASSERT_EQ(pktFormat.l4_dst_port, utility::kUdfL4DstPort);
  };

  verifyAcrossWarmBoots(setupUdfConfig, verifyUdfConfig);
};

TEST_F(BcmUdfTest, deleteUdfConfiguration) {
  // Udf Config
  applyNewState(setupUdfConfiguration(true));

  const int udfGroupId = getHwSwitch()->getUdfMgr()->getBcmUdfGroupId(
      utility::kUdfHashDstQueuePairGroupName);
  const int udfPacketMatcherId =
      getHwSwitch()->getUdfMgr()->getBcmUdfPacketMatcherId(
          utility::kUdfL4UdpRocePktMatcherName);

  // Undo Udf Config
  applyNewState(setupUdfConfiguration(false));

  auto verifyUdfConfig = [=]() {
    EXPECT_THROW(
        getHwSwitch()->getUdfMgr()->getBcmUdfGroupId(
            utility::kUdfHashDstQueuePairGroupName),
        FbossError);
    EXPECT_THROW(
        getHwSwitch()->getUdfMgr()->getBcmUdfPacketMatcherId(
            utility::kUdfL4UdpRocePktMatcherName),
        FbossError);

    /* get udf info */
    bcm_udf_t udfInfo;
    bcm_udf_t_init(&udfInfo);
    auto rv = bcm_udf_get(getHwSwitch()->getUnit(), udfGroupId, &udfInfo);
    if (getAsic()->getAsicType() != cfg::AsicType::ASIC_TYPE_FAKE) {
      EXPECT_THROW(
          bcmCheckError(
              rv, "Unable to get udfInfo for udfGroupId: ", udfGroupId),
          FbossError);
    }

    /* get udf pkt info */
    bcm_udf_pkt_format_info_t pktFormat;
    bcm_udf_pkt_format_info_t_init(&pktFormat);
    rv = bcm_udf_pkt_format_info_get(
        getHwSwitch()->getUnit(), udfPacketMatcherId, &pktFormat);
    if (getAsic()->getAsicType() != cfg::AsicType::ASIC_TYPE_FAKE) {
      EXPECT_THROW(
          bcmCheckError(
              rv,
              "Unable to get pkt_format for udfPacketMatcherId: ",
              udfPacketMatcherId),
          FbossError);
    }
  };

  verifyAcrossWarmBoots([] {}, verifyUdfConfig);
};
} // namespace facebook::fboss
