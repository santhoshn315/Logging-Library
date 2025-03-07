#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <zlib.h>  // For compression
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

#define BUFFER_SIZE 1024  // Must be a power of 2
#define MAX_LOG_SIZE 1024 * 1024  // 1MB max log file size
#define MAX_LOG_FILES 5  // Keep last 5 log files
#define BATCH_SIZE 10  // Number of logs per network batch

class Logger {
public:
    enum Level { INFO, DEBUG, ERROR };
    enum LogFormat { PLAIN_TEXT, JSON };

    Logger(const string& filename, Level minLogLevel = INFO, LogFormat format = PLAIN_TEXT, bool networkLogging = false, string serverIp = "127.0.0.1", int serverPort = 5000)
        : baseFilename(filename), stopLogging(false), head(0), tail(0), currentLogSize(0), minLevel(minLogLevel), logFormat(format), enableNetworkLogging(networkLogging), serverIP(serverIp), serverPort(serverPort) {
        rotateLogs();
        logFile.open(baseFilename, ios::out | ios::app);
        workerThread = thread(&Logger::processLogs, this);
        if (enableNetworkLogging) {
            networkThread = thread(&Logger::sendLogsToServer, this);
        }
    }

    ~Logger() {
        stopLogging = true;
        if (workerThread.joinable()) {
            workerThread.join();
        }
        if (networkThread.joinable()) {
            networkThread.join();
        }
        if (logFile.is_open()) {
            logFile.close();
        }
    }

    void log(Level level, const string& message) {
        if (level < minLevel) return; // Log filtering

        stringstream logEntry;
        logEntry << formatLog(level, message);

        size_t next = (head.load(memory_order_relaxed) + 1) % BUFFER_SIZE;
        if (next == tail.load(memory_order_acquire)) {
            return; // Drop log if buffer is full
        }
        
        buffer[head.load(memory_order_relaxed)] = logEntry.str();
        head.store(next, memory_order_release);
    }

private:
    string baseFilename;
    ofstream logFile;
    vector<string> buffer{BUFFER_SIZE};
    atomic<size_t> head, tail;
    thread workerThread, networkThread;
    atomic<bool> stopLogging;
    atomic<size_t> currentLogSize;
    Level minLevel;
    LogFormat logFormat;
    bool enableNetworkLogging;
    string serverIP;
    int serverPort;

    void processLogs() {
        while (!stopLogging || head.load(memory_order_acquire) != tail.load(memory_order_acquire)) {
            if (head.load(memory_order_acquire) == tail.load(memory_order_acquire)) {
                this_thread::yield();
                continue;
            }

            size_t index = tail.load(memory_order_relaxed);
            string logMessage = buffer[index];
            tail.store((index + 1) % BUFFER_SIZE, memory_order_release);
            
            cout << logMessage;
            logFile << logMessage;
            logFile.flush();
            
            currentLogSize += logMessage.size();
            if (currentLogSize >= MAX_LOG_SIZE) {
                rotateLogs();
            }
        }
    }

    void sendLogsToServer() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);

        if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            close(sock);
            return;
        }

        vector<string> batch;
        while (!stopLogging || head.load(memory_order_acquire) != tail.load(memory_order_acquire)) {
            if (head.load(memory_order_acquire) == tail.load(memory_order_acquire)) {
                this_thread::yield();
                continue;
            }

            size_t index = tail.load(memory_order_relaxed);
            batch.push_back(buffer[index]);
            tail.store((index + 1) % BUFFER_SIZE, memory_order_release);

            if (batch.size() >= BATCH_SIZE) {
                string batchMessage;
                for (const auto& log : batch) {
                    batchMessage += log + "\n";
                }
                send(sock, batchMessage.c_str(), batchMessage.size(), 0);
                batch.clear();
            }
        }
        if (!batch.empty()) {
            string batchMessage;
            for (const auto& log : batch) {
                batchMessage += log + "\n";
            }
            send(sock, batchMessage.c_str(), batchMessage.size(), 0);
        }
        close(sock);
    }
};

int main() {
    Logger logger("logs.txt", Logger::DEBUG, Logger::JSON, true, "127.0.0.1", 5000);

    logger.log(Logger::INFO, "This is an info message.");
    logger.log(Logger::DEBUG, "Debugging application.");
    logger.log(Logger::ERROR, "An error occurred!");

    this_thread::sleep_for(chrono::seconds(2)); // Ensure logs are written
    return 0;
}
