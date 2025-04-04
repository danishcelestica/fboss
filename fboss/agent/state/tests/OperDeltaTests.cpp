/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/FbossError.h"
#include "fboss/agent/FsdbHelper.h"
#include "fboss/agent/HwSwitchMatcher.h"
#include "fboss/agent/gen-cpp2/switch_config_constants.h"
#include "fboss/agent/hw/mock/MockPlatform.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/test/TestUtils.h"
#include "fboss/fsdb/common/Utils.h"

#include "fboss/agent/state/DeltaFunctions.h"

#include <boost/container/flat_map.hpp>
#include <gtest/gtest.h>

using namespace facebook::fboss;
using boost::container::flat_map;
using std::make_pair;
using std::make_shared;
using std::shared_ptr;

TEST(OperDeltaTests, OperDeltaCompute) {
  auto platform = createMockPlatform();
  auto emptyConfig = cfg::SwitchConfig();
  auto stateV0 = publishAndApplyConfig(
      make_shared<SwitchState>(), &emptyConfig, platform.get());
  addSwitchInfo(stateV0);
  auto config = testConfigA();

  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);

  auto delta1 = StateDelta(stateV0, stateV1);
  // 1 port map, 1 vlan map, 1 intf map change
  EXPECT_EQ(delta1.getOperDelta().changes()->size(), 3);

  auto stateV2 = stateV1->clone();
  auto mnpuMirrors = stateV1->getMirrors()->modify(&stateV2);
  state::MirrorFields mirror{};
  mirror.name() = "mirror0";
  mirror.configHasEgressPort() = true;
  mirror.egressPort() = 1;
  mirror.egressPortDesc() = PortDescriptor(PortID(1)).toThrift();
  mirror.isResolved() = true;
  auto mirrorObj = std::make_shared<Mirror>(mirror);
  HwSwitchMatcher scope(std::unordered_set<SwitchID>({SwitchID(0)}));
  mnpuMirrors->addNode(mirrorObj, scope);
  stateV2->publish();

  auto delta2 = StateDelta(stateV1, stateV2);
  // 1 mirror
  EXPECT_EQ(delta2.getOperDelta().changes()->size(), 1);
}

TEST(OperDeltaTests, OperDeltaProcess) {
  auto platform = createMockPlatform();
  auto stateV0 = make_shared<SwitchState>();
  auto config = testConfigA();
  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);

  auto operDelta = fsdb::computeOperDelta(stateV0, stateV1, {}, true);

  auto delta = StateDelta(stateV0, operDelta);
  auto stateV2 = delta.newState();
  EXPECT_EQ(stateV2->toThrift(), stateV1->toThrift());
  EXPECT_EQ(stateV0->getGeneration(), 0);
  EXPECT_EQ(stateV1->getGeneration(), 1);
  EXPECT_EQ(stateV2->getGeneration(), 1);
}

TEST(OperDeltaTests, DeltaCompare) {
  auto platform = createMockPlatform();
  auto stateV0 = make_shared<SwitchState>();
  auto config = testConfigA();
  auto stateV1 = publishAndApplyConfig(stateV0, &config, platform.get());
  ASSERT_NE(nullptr, stateV1);

  auto operDelta = fsdb::computeOperDelta(stateV0, stateV1, {}, true);
  auto delta1 = StateDelta(stateV0, operDelta);
  auto delta2 = StateDelta(stateV0, operDelta);
  auto delta3 = StateDelta(delta1.newState(), delta2.newState());
  // port delta processing happens here
  EXPECT_FALSE(DeltaFunctions::isEmpty(delta3.getPortsDelta()));
  {
    DeltaComparison::PolicyRAII policy(DeltaComparison::Policy::DEEP);
    // port delta processing does not here as, deep comparison between same
    // objects
    EXPECT_TRUE(DeltaFunctions::isEmpty(delta3.getPortsDelta()));
  }
}
