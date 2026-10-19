# Suppressed function inventory

Generated from `offline-installed.json` for `com.ea.game.pvz2_row` version 13.5.1.

These are explicit disabled bindings, not unresolved imports. Calls count only this session; zero means installed but unused. Local setup, cleanup and preference/storage implementations remain active. No paid item or ad reward is fabricated.

The implementation audit and compatibility boundaries are in [offline-compatibility.md](offline-compatibility.md). Runtime JSON also includes the exact importing libraries and binding status under `native_imports`.

## online-advertising (36)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `csdk/gluads/IAdvertising.destroy()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.discard(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.getReward(Ljava/lang/String;Ljava/lang/String;)Lcsdk/gluads/Reward;` | null; no ad reward | 0 |
| `csdk/gluads/IAdvertising.internal_onNewSession()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.internal_setGemsData(Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.internal_updateAdsFrequency(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.internal_updateCrossPromotion(Ljava/lang/String;Ljava/util/Map;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.internal_updatePrivacyStatus(Lcsdk/gluads/PrivacyStatus;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.internal_updateRewardList(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.isLoaded(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)Z` | false; no ad loaded | 0 |
| `csdk/gluads/IAdvertising.load(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Unavailable placement callback; no ad or reward | 0 |
| `csdk/gluads/IAdvertising.onAdvertisementClick(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.onAdvertisementImpression(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.onPause()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.onResume()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.setCustomProperties(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.setUserIdentifier(Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/IAdvertising.show(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Unavailable placement callback; no ad or reward | 0 |
| `csdk/gluads/impl/DummyAdvertising.destroy()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.discard(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.getReward(Ljava/lang/String;Ljava/lang/String;)Lcsdk/gluads/Reward;` | null; no ad reward | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_onNewSession()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_setGemsData(Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_updateAdsFrequency(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_updateCrossPromotion(Ljava/lang/String;Ljava/util/Map;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_updatePrivacyStatus(Lcsdk/gluads/PrivacyStatus;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.internal_updateRewardList(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.isLoaded(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)Z` | false; no ad loaded | 0 |
| `csdk/gluads/impl/DummyAdvertising.load(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Unavailable placement callback; no ad or reward | 5 |
| `csdk/gluads/impl/DummyAdvertising.onAdvertisementClick(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.onAdvertisementImpression(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.onPause()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.onResume()V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.setCustomProperties(Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.setUserIdentifier(Ljava/lang/String;Ljava/util/Map;)V` | SDK DummyAdvertising no-op; no ad request | 0 |
| `csdk/gluads/impl/DummyAdvertising.show(Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Unavailable placement callback; no ad or reward | 0 |

## online-analytics (58)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `com/glu/plugins/gluanalytics/Analytics.destroy()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.init(Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_firebaseAllowPersonalizedAds(Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_firebaseInitialize(Ljava/util/Map;Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_firebaseUpdateUnderAgeUserStatus(Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_logCpuEvent(Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_logEvent_adImpression(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/math/BigDecimal;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_logEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Long;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_logFirebaseEvent(Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_perfEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_pinpointAttribute(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_setRevID(Ljava/lang/String;Ljava/lang/String;Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_trackRevenueInUsd(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.internal_updatePSValue(Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logAdvertisementOpportunity(Ljava/lang/String;Ljava/lang/String;JZLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logCurrencySink(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logCurrencySource(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Long;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 9 |
| `com/glu/plugins/gluanalytics/Analytics.logInAppPurchaseInCents(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logInAppPurchaseInUsd(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logMissionCompleted(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logMissionFailed(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logMissionStarted(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.logTutorialStepCompleted(Ljava/lang/String;JLjava/lang/String;JLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.onPause()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.onResume()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.perfSampleEnd(Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.perfSampleStart(Ljava/lang/String;ILjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/Analytics.setUserIdentifier(Ljava/lang/String;)V` | Disabled SDK backend: no analytics delivery | 2 |
| `com/glu/plugins/gluanalytics/IAnalytics.destroy()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.init(Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_firebaseAllowPersonalizedAds(Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_firebaseInitialize(Ljava/util/Map;Lcom/glu/plugins/gluanalytics/IAnalyticsCallback;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_firebaseUpdateUnderAgeUserStatus(Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_logCpuEvent(Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_logEvent_adImpression(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/math/BigDecimal;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_logEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Long;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_logFirebaseEvent(Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_perfEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_pinpointAttribute(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_setRevID(Ljava/lang/String;Ljava/lang/String;Z)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_trackRevenueInUsd(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.internal_updatePSValue(Ljava/lang/String;ZLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logAdvertisementOpportunity(Ljava/lang/String;Ljava/lang/String;JZLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logCurrencySink(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logCurrencySource(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logEvent(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Long;Ljava/lang/Long;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logInAppPurchaseInCents(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logInAppPurchaseInUsd(DLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logMissionCompleted(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logMissionFailed(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logMissionStarted(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJJLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.logTutorialStepCompleted(Ljava/lang/String;JLjava/lang/String;JLjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.onPause()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.onResume()V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.perfSampleEnd(Ljava/lang/String;Ljava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.perfSampleStart(Ljava/lang/String;ILjava/util/Map;)V` | Disabled SDK backend: no analytics delivery | 0 |
| `com/glu/plugins/gluanalytics/IAnalytics.setUserIdentifier(Ljava/lang/String;)V` | Disabled SDK backend: no analytics delivery | 0 |

## online-billing (12)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `com/ea/nimble/mtx/INimbleMTX.finalizeTransaction(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.getAvailableCatalogItems()Ljava/util/List;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 2 |
| `com/ea/nimble/mtx/INimbleMTX.getPendingTransactions()Ljava/util/List;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.getPurchasedTransactions()Ljava/util/List;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.getRecoveredTransactions()Ljava/util/List;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.getTransaction(Ljava/lang/String;)Lcom/ea/nimble/mtx/NimbleMTXTransaction;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.itemGranted(Ljava/lang/String;Lcom/ea/nimble/mtx/NimbleCatalogItem$ItemType;Lcom/ea/nimble/mtx/INimbleMTX$ItemGrantedCallback;)Lcom/ea/nimble/Error;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.purchaseItem(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Ljava/lang/String;)Lcom/ea/nimble/Error;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.refreshAvailableCatalogItems()V` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 3 |
| `com/ea/nimble/mtx/INimbleMTX.restorePurchasedTransactions()V` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.resumeTransaction(Ljava/lang/String;Lcom/ea/nimble/mtx/INimbleMTX$PurchaseTransactionCallback;Lcom/ea/nimble/mtx/INimbleMTX$ItemGrantedCallback;Lcom/ea/nimble/mtx/INimbleMTX$FinalizeTransactionCallback;)Lcom/ea/nimble/Error;` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |
| `com/ea/nimble/mtx/INimbleMTX.setPlatformParameters(Ljava/util/Map;)V` | Billing unavailable: empty transactions/catalog or Nimble NOT_AVAILABLE (103); no grants | 0 |

## online-google-play (10)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements.Play_QueueAchievement_Percentage(Ljava/lang/String;F)V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements.Play_ResetAchievements()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements.Play_ShowAchievementView()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect.Play_Connect_Silent()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect.Play_Connect()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect.Play_Disconnect()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect.Play_IsConnected()Z` | Signed out: false or immediate void return; no login, score or achievement request | 1899 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard.Play_ShowLeaderboardView(Ljava/lang/String;)V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard.Play_ShowLeaderboardViewAll()V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |
| `com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard.Play_SubmitScoreToLeaderboard(ILjava/lang/String;)V` | Signed out: false or immediate void return; no login, score or achievement request | 0 |

## online-http (2)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `com/popcap/SexyAppFramework/AndroidHttpTransaction.run()V` | One asynchronous HttpTransactionError callback per transaction; no request sent | 0 |
| `com/popcap/SexyAppFramework/AndroidHttpTransaction.Start()V` | One asynchronous HttpTransactionError callback per transaction; no request sent | 9 |

## online-network (28)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `accept` | -1 / ENETUNREACH; no socket or network request | 0 |
| `accept4` | -1 / ENETUNREACH; no socket or network request | 0 |
| `bind` | -1 / ENETUNREACH; no socket or network request | 0 |
| `com/ea/nimble/INetwork.forceRedetectNetworkStatus()V` | No-op; keep offline status | 0 |
| `com/ea/nimble/INetwork.getStatus()Lcom/ea/nimble/Network$Status;` | Network.Status.NONE; offline gameplay remains available | 0 |
| `com/ea/nimble/INetwork.isNetworkWifi()Z` | false; Android Wi-Fi unavailable | 0 |
| `com/ea/nimble/Network.getHttpProxy()Ljava/lang/String;` | null; no Android HTTP proxy | 0 |
| `com/ea/nimble/NetworkImpl.forceRedetectNetworkStatus()V` | No-op; keep offline status | 0 |
| `com/ea/nimble/NetworkImpl.getStatus()Lcom/ea/nimble/Network$Status;` | Network.Status.NONE; offline gameplay remains available | 4 |
| `com/ea/nimble/NetworkImpl.isNetworkWifi()Z` | false; Android Wi-Fi unavailable | 0 |
| `connect` | -1 / ENETUNREACH; no socket or network request | 0 |
| `getaddrinfo` | Android EAI_NONAME (8), null result; no DNS lookup | 0 |
| `gethostbyname` | null; no DNS lookup | 0 |
| `getnameinfo` | Android EAI_NONAME (8); no DNS lookup | 0 |
| `getpeername` | -1 / ENETUNREACH; no socket or network request | 0 |
| `getsockname` | -1 / ENETUNREACH; no socket or network request | 0 |
| `getsockopt` | -1 / ENETUNREACH; no socket or network request | 0 |
| `if_nametoindex` | 0 / ENODEV; no Android network interface | 0 |
| `listen` | -1 / ENETUNREACH; no socket or network request | 0 |
| `recv` | -1 / ENETUNREACH; no socket or network request | 0 |
| `recvfrom` | -1 / ENETUNREACH; no socket or network request | 0 |
| `recvmmsg` | -1 / ENETUNREACH; no socket or network request | 0 |
| `send` | -1 / ENETUNREACH; no socket or network request | 0 |
| `sendmmsg` | -1 / ENETUNREACH; no socket or network request | 0 |
| `sendto` | -1 / ENETUNREACH; no socket or network request | 0 |
| `setsockopt` | -1 / ENETUNREACH; no socket or network request | 0 |
| `shutdown` | -1 / ENETUNREACH; no socket or network request | 0 |
| `socket` | -1 / ENETUNREACH; no socket or network request | 0 |

## optional-posix (6)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `chmod` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |
| `fchmod` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |
| `fchown` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |
| `lchown` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |
| `utime` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |
| `utimes` | -1 / ENOSYS; use fchmodat or utimensat for supported metadata | 0 |

## unrelated-posix (16)

| Function / Java signature | Behavior | Calls |
| --- | --- | --- |
| `__readlink_chk` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `execv` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `fork` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `ioctl` | -1 / ENOTTY; Android device-control requests are unavailable | 0 |
| `link` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `mkfifo` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `mknod` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `pipe` | -1 / ENOSYS; no host process, device, pipe or link is created | 1 |
| `ptrace` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `raise` | -1 / ENOSYS; no host signal is raised | 0 |
| `readlink` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `sigaction` | SIG_ERR or -1 / ENOSYS; host signal handlers remain untouched | 0 |
| `signal` | SIG_ERR or -1 / ENOSYS; host signal handlers remain untouched | 0 |
| `symlink` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `system` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
| `waitpid` | -1 / ENOSYS; no host process, device, pipe or link is created | 0 |
