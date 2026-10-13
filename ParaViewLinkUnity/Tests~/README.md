# Receiver test

`PVLinkReceiverTest.cs` drives MeshReceiver against a store written by the
DataManager's DMLink plugin test pipeline (project `Test/pv`: Wavelet contour,
slice isolines, masked points over 5 timesteps). Copy it into a scratch
project's `Assets/Editor/` (manifest: this package, com.tacc.datamanager,
URP, Input System), then:

    DM_TEST_STORE=<store> Unity -batchmode -nographics -projectPath <proj> \
      -executeMethod PVLinkReceiverTest.Run -logFile log.txt
