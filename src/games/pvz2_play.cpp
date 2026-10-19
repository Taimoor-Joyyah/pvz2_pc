#include <acp/pvz2.hpp>
namespace acp::games {
void install_pvz2_play(android::Runtime &r) {
    auto &j = *r.jni;
    constexpr auto connect = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect";
    constexpr auto achievements = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements";
    constexpr auto leaderboards = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard";
    // Achievement and leaderboard operations return immediately while signed out.
    // Connection requests stay disabled in the desktop runtime.
    j.hook(connect, "Play_IsConnected", "()Z", [](auto &) { return false; });
    for (auto name : {"Play_Connect", "Play_Connect_Silent", "Play_Disconnect"})
        j.hook(connect, name, "()V", [](auto &) { return 0; });
    j.hook(achievements, "Play_QueueAchievement_Percentage", "(Ljava/lang/String;F)V",
           [](auto &) { return 0; });
    for (auto name : {"Play_ResetAchievements", "Play_ShowAchievementView"})
        j.hook(achievements, name, "()V", [](auto &) { return 0; });
    j.hook(leaderboards, "Play_ShowLeaderboardView", "(Ljava/lang/String;)V", [](auto &) { return 0; });
    j.hook(leaderboards, "Play_ShowLeaderboardViewAll", "()V", [](auto &) { return 0; });
    j.hook(leaderboards, "Play_SubmitScoreToLeaderboard", "(ILjava/lang/String;)V",
           [](auto &) { return 0; });
    r.message("[PopCap] Google Play Games disabled; using local gameplay and saves");
}
} // namespace acp::games
