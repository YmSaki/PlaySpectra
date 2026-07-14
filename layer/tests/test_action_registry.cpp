#include <gtest/gtest.h>
#include "action_registry.h"

using namespace vr_agent;

/*
このテストは `ActionReg` 構造体へのデータの格納と取り出し、
および `PathToStr` 等で文字列化されたパスの正確な保持を検証します。
理由: 複数の `subactionPaths` (Left, Rightなど) を持つアクションが、
内部で欠損なく全て記録されているかを確かめるため。
*/
TEST(ActionRegistryTest, RecordAndRetrieveActionData) {
    ActionReg reg;
    reg.actionSet = reinterpret_cast<XrActionSet>(0x1234);
    reg.name = "grab_object";
    reg.localizedName = "Grab Object";
    reg.type = XR_ACTION_TYPE_FLOAT_INPUT;
    reg.subactionPaths.push_back("/user/hand/left");
    reg.subactionPaths.push_back("/user/hand/right");

    EXPECT_EQ(reg.subactionPaths.size(), 2);
    EXPECT_EQ(reg.subactionPaths[0], "/user/hand/left");
    EXPECT_EQ(reg.type, XR_ACTION_TYPE_FLOAT_INPUT);
}
