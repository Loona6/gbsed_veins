#include "veins/modules/application/gbsed/GBSEDApp.h"
#include "veins/modules/application/gbsed/GBSEDMessage_m.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <cmath>
#include <ostream>

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
        startTime = par("startTime").doubleValue();
        sendInterval = par("sendInterval").doubleValue();
        writeCsvLog = par("writeCsvLog").boolValue();
        writePartialFiles = par("writePartialFiles").boolValue();

        // logChunk() appends, so a stale log from an earlier run would merge
        // into this one's. Each node clears only the file it writes.
        if (writeCsvLog) {
            std::error_code ec;
            std::filesystem::path dir(outputDir.empty() ? "." : outputDir);
            std::filesystem::remove(dir / (isSender ? "tx_log.csv" : "rx_log.csv"), ec);
        }

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
            scheduleAt(simTime() + startTime, sendChunkEvt);
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
                scheduleAt(simTime() + sendInterval, sendChunkEvt);
            }
            else {
                EV_INFO << "GBSEDApp: all " << filePaths.size()
                        << " file(s) sent. Sender finished." << endl;
                cancelEvent(sendChunkEvt);
            }
            return;
        }

        scheduleAt(simTime() + sendInterval, sendChunkEvt);
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

    // Carried so the receiver can work out how far apart the two vehicles were
    // when this chunk went out; without it, a lost chunk has no distance.
    msg->setTxPosX(curPosition.x);
    msg->setTxPosY(curPosition.y);

    sendDown(msg);
    chunksSentCount++;
    logChunk("tx_log.csv", fname, nextChunkToSend, totalChunks,
        curPosition.x, curPosition.y, NAN, NAN, NAN);
    EV_INFO << "GBSEDApp: sent chunk " << nextChunkToSend << "/" << totalChunks
            << " of '" << fname << "' from (" << curPosition.x << ", "
            << curPosition.y << ")" << endl;
}

void GBSEDApp::logChunk(const std::string& file, const std::string& fileName, int chunkIndex,
    int totalChunksForFile, double txX, double txY, double rxX, double rxY, double distance)
{
    if (!writeCsvLog) return;

    std::filesystem::path dir(outputDir.empty() ? "." : outputDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        EV_WARN << "GBSEDApp: cannot create '" << dir.string()
                << "' for the CSV log: " << ec.message() << endl;
        return;
    }

    std::filesystem::path path = dir / file;
    bool needHeader = !std::filesystem::exists(path);
    std::ofstream out(path, std::ios::app);
    if (!out) {
        EV_WARN << "GBSEDApp: cannot append to " << path.string() << endl;
        return;
    }
    if (needHeader) {
        out << "fileName,chunkIndex,totalChunks,simTime,txX,txY,rxX,rxY,distance\n";
    }
    // NAN means "not applicable on this side" and is written as an empty cell.
    auto cell = [](std::ostream& os, double v) -> std::ostream& {
        if (!std::isnan(v)) os << v;
        return os;
    };
    out << fileName << ',' << chunkIndex << ',' << totalChunksForFile << ','
        << simTime().dbl() << ',';
    cell(out, txX) << ',';
    cell(out, txY) << ',';
    cell(out, rxX) << ',';
    cell(out, rxY) << ',';
    cell(out, distance) << '\n';
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
            // Hand it over anyway: with a slice-aligned payload the blocks
            // that did arrive still decode. Dropping it here is what made
            // partial delivery indistinguishable from total loss.
            if (writePartialFiles && chunksReceivedCount > 0) {
                writeReceivedFile(true);
                filesPartialCount++;
            }
        }
        startNewReceivedFile(incomingFile, gmsg->getTotalSize(), gmsg->getTotalChunks());
    }

    Coord txPos(gmsg->getTxPosX(), gmsg->getTxPosY(), curPosition.z);
    double distance = curPosition.distance(txPos);
    chunksHeardCount++;
    logChunk("rx_log.csv", incomingFile, gmsg->getChunkIndex(), gmsg->getTotalChunks(),
        txPos.x, txPos.y, curPosition.x, curPosition.y, distance);

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

void GBSEDApp::writeReceivedFile(bool partial)
{
    // outputDir may or may not carry a trailing separator; normalise before joining.
    std::filesystem::path dir(outputDir.empty() ? "." : outputDir);
    std::filesystem::path outPath = dir / ("received_" + currentReceivingFile);

    // The directory is not guaranteed to exist. Without this, opening the stream
    // below fails and the file is dropped without any indication of the loss.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw cRuntimeError("GBSEDApp: could not create output directory '%s': %s",
            dir.c_str(), ec.message().c_str());
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        throw cRuntimeError("GBSEDApp: could not open '%s' for writing", outPath.c_str());
    }
    out.write((const char*)receivedBuffer.data(), receivedBuffer.size());
    out.close();
    if (!out) {
        throw cRuntimeError("GBSEDApp: failed while writing '%s'", outPath.c_str());
    }

    EV_INFO << "GBSEDApp: wrote " << (partial ? "PARTIAL " : "")
            << "received file to " << outPath.string() << " ("
            << receivedBuffer.size() << " bytes"
            << (partial ? ", " + std::to_string(chunksReceivedCount) + "/"
                          + std::to_string(chunkReceived.size()) + " chunks"
                        : "")
            << ")" << endl;
}

void GBSEDApp::finish()
{
    DemoBaseApplLayer::finish();
    if (sendChunkEvt) {
        cancelAndDelete(sendChunkEvt);
        sendChunkEvt = nullptr;
    }
    if (isSender) {
        EV_INFO << "GBSEDApp: sender finished. Chunks transmitted: " << chunksSentCount
                << " of " << filePaths.size() << " queued file(s); reached file "
                << currentFileIndex << "." << endl;
        recordScalar("chunksSent", chunksSentCount);
        recordScalar("filesQueued", (long)filePaths.size());
        recordScalar("filesStarted", currentFileIndex);
    }
    else {
        // The last file in the queue never triggers a file switch, so an
        // incomplete one would otherwise be dropped here.
        if (writePartialFiles && !currentReceivingFile.empty()
            && !fileWritten && chunksReceivedCount > 0) {
            writeReceivedFile(true);
            filesPartialCount++;
        }
        EV_INFO << "GBSEDApp: receiver finished. Files completed: " << filesReceivedCount
                << "; partial: " << filesPartialCount
                << "; chunks heard: " << chunksHeardCount << endl;
        recordScalar("filesReceived", filesReceivedCount);
        recordScalar("filesPartial", filesPartialCount);
        recordScalar("chunksHeard", chunksHeardCount);
    }
}