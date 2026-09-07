#include "veins/modules/application/gbsed/GBSEDApp.h"
#include "veins/modules/application/gbsed/GBSEDMessage_m.h"
#include <fstream>
#include <algorithm>
#include <sstream>

using namespace veins;

Define_Module(veins::GBSEDApp);

static const std::string b64chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

std::string GBSEDApp::base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out.push_back(b64chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(b64chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<uint8_t> GBSEDApp::base64Decode(const std::string& encoded)
{
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[(unsigned char)b64chars[i]] = i;

    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::vector<std::string> GBSEDApp::splitPaths(const std::string& s, char delim)
{
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        // trim whitespace
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end = item.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue; // empty/whitespace-only entry
        result.push_back(item.substr(start, end - start + 1));
    }
    return result;
}

std::string GBSEDApp::basenameOf(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

void GBSEDApp::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);
    if (stage == 0) {
        isSender = par("isSender").boolValue();
        outputDir = par("outputDir").stdstringValue();
        chunkSizeBytes = par("chunkSize").intValue();

        if (isSender) {
            std::string rawPaths = par("filePath").stdstringValue();
            filePaths = splitPaths(rawPaths);

            if (filePaths.empty()) {
                EV_ERROR << "GBSEDApp: no valid file paths given in 'filePath'" << endl;
                return;
            }

            EV_INFO << "GBSEDApp: queued " << filePaths.size() << " file(s) to send" << endl;

            currentFileIndex = 0;
            loadFile();

            sendChunkEvt = new cMessage("sendChunkEvt");
            // Wait well past typical TraCI vehicle-creation delay before first send
            scheduleAt(simTime() + 10.0, sendChunkEvt);
        }
    }
}

void GBSEDApp::loadFile()
{
    const std::string& path = filePaths[currentFileIndex];

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        EV_ERROR << "GBSEDApp: could not open file " << path << endl;
        fileBuffer.clear();
        totalChunks = 0;
        return;
    }
    fileBuffer.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    nextChunkToSend = 0;
    totalChunks = (fileBuffer.size() + chunkSizeBytes - 1) / chunkSizeBytes;
    EV_INFO << "GBSEDApp: loaded '" << path << "' (" << fileBuffer.size()
            << " bytes, " << totalChunks << " chunks) ["
            << (currentFileIndex + 1) << "/" << filePaths.size() << "]" << endl;
}

void GBSEDApp::handleSelfMsg(cMessage* msg)
{
    if (msg == sendChunkEvt) {
        if (totalChunks > 0) {
            sendNextChunk();
            nextChunkToSend++;
        }

        if (nextChunkToSend >= totalChunks) {
            // Current file fully sent (at least once). Move to the next file.
            currentFileIndex++;
            if (currentFileIndex < (int)filePaths.size()) {
                loadFile();
                scheduleAt(simTime() + 2.0, sendChunkEvt);
            }
            else {
                EV_INFO << "GBSEDApp: all " << filePaths.size()
                        << " file(s) sent. Sender finished." << endl;
                cancelEvent(sendChunkEvt);
            }
            return;
        }

        scheduleAt(simTime() + 2.0, sendChunkEvt);
        return;
    }
    DemoBaseApplLayer::handleSelfMsg(msg);
}

void GBSEDApp::sendNextChunk()
{
    if (totalChunks == 0) return;

    int offset = nextChunkToSend * chunkSizeBytes;
    int len = std::min(chunkSizeBytes, (int)fileBuffer.size() - offset);

    GBSEDMessage* msg = new GBSEDMessage();
    populateWSM(msg);
    msg->setChunkIndex(nextChunkToSend);
    msg->setTotalChunks(totalChunks);
    msg->setTotalSize((int)fileBuffer.size());

    std::string encoded = base64Encode(&fileBuffer[offset], len);
    msg->setChunkData(encoded.c_str());

    std::string fname = basenameOf(filePaths[currentFileIndex]);
    msg->setFileName(fname.c_str());
    msg->setIsLastFile(currentFileIndex == (int)filePaths.size() - 1);

    sendDown(msg);
    EV_INFO << "GBSEDApp: sent chunk " << nextChunkToSend << "/" << totalChunks
            << " of '" << fname << "'" << endl;
}

void GBSEDApp::startNewReceivedFile(const std::string& fileName, int totalSize, int totalChunksForFile)
{
    currentReceivingFile = fileName;
    receivedBuffer.assign(totalSize, 0);
    chunkReceived.assign(totalChunksForFile, false);
    chunksReceivedCount = 0;
    fileWritten = false;
    EV_INFO << "GBSEDApp: starting new incoming file '" << fileName
            << "' (" << totalSize << " bytes, " << totalChunksForFile << " chunks)" << endl;
}

void GBSEDApp::onWSM(BaseFrame1609_4* wsm)
{
    GBSEDMessage* gmsg = dynamic_cast<GBSEDMessage*>(wsm);
    if (!gmsg || isSender) return;

    std::string incomingFile = gmsg->getFileName();

    // New file detected (first-ever message, or filename changed from what we were assembling)
    if (currentReceivingFile.empty() || incomingFile != currentReceivingFile) {
        // If we were mid-file and it never completed, log that clearly before switching.
        if (!currentReceivingFile.empty() && !fileWritten) {
            EV_WARN << "GBSEDApp: switching to file '" << incomingFile
                    << "' but previous file '" << currentReceivingFile
                    << "' was never fully received (" << chunksReceivedCount
                    << "/" << chunkReceived.size() << " chunks)" << endl;
        }
        startNewReceivedFile(incomingFile, gmsg->getTotalSize(), gmsg->getTotalChunks());
    }

    int idx = gmsg->getChunkIndex();
    if (idx < (int)chunkReceived.size() && !chunkReceived[idx]) {
        std::vector<uint8_t> decoded = base64Decode(gmsg->getChunkData());
        int offset = idx * chunkSizeBytes;
        std::copy(decoded.begin(), decoded.end(), receivedBuffer.begin() + offset);
        chunkReceived[idx] = true;
        chunksReceivedCount++;
        EV_INFO << "GBSEDApp: received chunk " << idx << "/" << gmsg->getTotalChunks()
                << " of '" << incomingFile << "'" << endl;

        if (chunksReceivedCount == gmsg->getTotalChunks() && !fileWritten) {
            writeReceivedFile();
            fileWritten = true;
            filesReceivedCount++;

            if (gmsg->isLastFile()) {
                EV_INFO << "GBSEDApp: last file in queue received. Total files completed: "
                        << filesReceivedCount << endl;
            }
        }
    }
}

void GBSEDApp::writeReceivedFile()
{
    std::string outPath = outputDir + "/received_" + currentReceivingFile;
    std::ofstream out(outPath, std::ios::binary);
    out.write((char*)receivedBuffer.data(), receivedBuffer.size());
    out.close();
    EV_INFO << "GBSEDApp: wrote received file to " << outPath << endl;
}

void GBSEDApp::finish()
{
    DemoBaseApplLayer::finish();
    if (sendChunkEvt) {
        cancelAndDelete(sendChunkEvt);
        sendChunkEvt = nullptr;
    }
    if (!isSender) {
        EV_INFO << "GBSEDApp: receiver finished. Files completed: " << filesReceivedCount << endl;
    }
}