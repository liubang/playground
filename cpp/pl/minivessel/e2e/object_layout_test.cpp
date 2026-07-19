#include <gtest/gtest.h>

#include "cpp/pl/minivessel/e2e/object_layout.h"

namespace pl::minivessel::e2e {

TEST(ObjectLayoutTest, EscapesGroupAndBuildsStableRecordPaths) {
    GroupIdentity group{.group_id = "counter/../blue team", .incarnation = GroupIncarnation(7)};
    auto path =
        RecordObjectPath(group, Lrsn(42), WriterEpoch(3), LogRecordType::kMutation, "add/1");
    ASSERT_TRUE(path.ok()) << path.status();
    EXPECT_EQ(path->find("/../"), std::string::npos);
    EXPECT_NE(path->find("/counter%2F..%2Fblue%20team/7/wal/records/"), std::string::npos);
    EXPECT_NE(path->find("00000000000000000042-e3-"), std::string::npos);
    EXPECT_TRUE(path->ends_with(".record"));
    EXPECT_EQ(
        *path,
        *RecordObjectPath(group, Lrsn(42), WriterEpoch(3), LogRecordType::kMutation, "add/1"));
}

TEST(ObjectLayoutTest, SeparatesCheckpointObjects) {
    GroupIdentity group{.group_id = "counter", .incarnation = GroupIncarnation(1)};
    auto path = RecordObjectPath(group, Lrsn(9), WriterEpoch(2), LogRecordType::kCheckpoint, {});
    ASSERT_TRUE(path.ok());
    EXPECT_NE(path->find("/wal/checkpoints/"), std::string::npos);
    EXPECT_TRUE(path->ends_with(".checkpoint"));
}

TEST(ObjectLayoutTest, RejectsUnsafeDegenerateSegments) {
    EXPECT_FALSE(EscapePathSegment("..").ok());
    EXPECT_FALSE(EscapePathSegment("").ok());
    EXPECT_FALSE(GroupRoot({.group_id = "x", .incarnation = GroupIncarnation(0)}).ok());
}

} // namespace pl::minivessel::e2e
