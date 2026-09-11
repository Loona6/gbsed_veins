package org.car2x.veins.modules.application.gbsed;

import org.car2x.veins.modules.application.ieee80211p.DemoBaseApplLayer;

simple GBSEDApp extends DemoBaseApplLayer
{
    parameters:
        @class(veins::GBSEDApp);
        bool isSender = default(false);
        string filePath = default("");
        string outputDir = default("received/");
        int chunkSize = default(1000);

        // When the first chunk goes out. Must be past TraCI vehicle creation.
        double startTime @unit(s) = default(10s);
        // Gap between consecutive chunks. The sender only exists while its
        // SUMO vehicle does, so this and the route length set how many
        // chunks can physically be transmitted in one run.
        double sendInterval @unit(s) = default(2s);

        // Append one row per sent/received chunk to tx_log.csv / rx_log.csv
        // in outputDir, so delivery can be correlated with distance.
        bool writeCsvLog = default(true);
}
