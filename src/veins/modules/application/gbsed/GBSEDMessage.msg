import veins.modules.messages.BaseFrame1609_4;

namespace veins;

packet GBSEDMessage extends BaseFrame1609_4 {
    int chunkIndex;
    int totalChunks;
    int totalSize;
    string chunkData;
    string fileName;      // basename of the file this chunk belongs to
    bool isLastFile = false; // true if this is the last file in the send queue
}
