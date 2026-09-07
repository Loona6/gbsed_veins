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
}
