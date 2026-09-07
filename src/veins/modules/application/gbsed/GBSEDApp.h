#pragma once

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include <vector>
#include <cstdint>
#include <string>

namespace veins {

class GBSEDApp : public DemoBaseApplLayer {
public:
    void initialize(int stage) override;
    void finish() override;

protected:
    void onWSM(BaseFrame1609_4* wsm) override;
    void handleSelfMsg(cMessage* msg) override;

private:
    bool isSender = false;

    // --- Sender: list of files to send, one after another ---
    std::vector<std::string> filePaths;
    int currentFileIndex = 0;

    std::string outputDir;
    int chunkSizeBytes = 1000;

    std::vector<uint8_t> fileBuffer;
    int totalChunks = 0;
    int nextChunkToSend = 0;

    // --- Receiver: tracks the file currently being assembled ---
    std::string currentReceivingFile;
    std::vector<uint8_t> receivedBuffer;
    std::vector<bool> chunkReceived;
    int chunksReceivedCount = 0;
    bool fileWritten = false;
    int filesReceivedCount = 0;

    cMessage* sendChunkEvt = nullptr;

    void loadFile();
    void sendNextChunk();
    void writeReceivedFile();
    void startNewReceivedFile(const std::string& fileName, int totalSize, int totalChunksForFile);

    static std::string base64Encode(const uint8_t* data, size_t len);
    static std::vector<uint8_t> base64Decode(const std::string& encoded);
    static std::vector<std::string> splitPaths(const std::string& s, char delim = ';');
    static std::string basenameOf(const std::string& path);
};

} // namespace veins
