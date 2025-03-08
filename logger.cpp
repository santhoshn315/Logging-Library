#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <queue>
#include <condition_variable>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std;

#define MAX_LOG_SIZE 1024 * 1024  // 1MB max log file size
#define MAX_LOG_FILES 5           // Keep last 5 log files
#define BATCH_SIZE 100            // Number of logs per batch (disk/network)
#define BUFFER_SIZE 1024          // Ring buffer size

class Logger {
public:
    enum Level { INFO, DEBUG, ERROR };
    enum LogFormat { PLAIN_TEXT, JSON };

    Logger(const string &filename, Level minLogLevel = INFO, LogFormat format = PLAIN_TEXT, 
           bool networkLogging = false, string serverIp = "127.0.0.1", int serverPort = 5000)
        : baseFilename(filename), stopLogging(false), minLevel(minLogLevel), logFormat(format), 
          enableNetworkLogging(networkLogging), serverIP(serverIp), serverPort(serverPort),
          head(0), tail(0), currentLogSize(0) 
    {
        rotateLogs();
        logFile.open(baseFilename, ios::out | ios::app);
        if (!logFile.is_open()) cerr << "Error: Failed to open log file: " << baseFilename << endl;

        workerThread = thread(&Logger::processLogs, this);
        if (enableNetworkLogging) networkThread = thread(&Logger::sendLogsToServer, this);

        cout << "Logger initialized. Writing logs to: " << baseFilename << endl;
    }

    ~Logger() {
        stopLogging = true;
        cv.notify_all();
        if (workerThread.joinable()) workerThread.join();
        if (networkThread.joinable()) networkThread.join();
        if (logFile.is_open()) logFile.close();
        cout << "Logger shutting down." << endl;
    }

    void log(Level level, const string &message) {
        if (level < minLevel) return; // Log filtering

        string formattedLog = formatLog(level, message);

        {
            unique_lock<mutex> lock(bufferMutex);
            if ((head + 1) % BUFFER_SIZE == tail) {
                cerr << "Buffer full, dropping log: " << formattedLog << endl;
                return; // Buffer full, drop log
            }
            buffer[head] = formattedLog;
            head = (head + 1) % BUFFER_SIZE;
        }

        cout << "Log added to buffer: " << formattedLog << endl;
        cv.notify_one(); // Wake up consumer thread
    }

private:
    string baseFilename;
    ofstream logFile;
    atomic<bool> stopLogging;
    Level minLevel;
    LogFormat logFormat;
    bool enableNetworkLogging;
    string serverIP;
    int serverPort;
    atomic<size_t> currentLogSize;

    string buffer[BUFFER_SIZE];
    atomic<size_t> head, tail;
    mutex bufferMutex;
    condition_variable cv;

    thread workerThread, networkThread;

    void processLogs() {
        vector<string> batch;
        cout << "Log processing thread started." << endl;

        while (!stopLogging || head != tail) {
            unique_lock<mutex> lock(bufferMutex);
            cv.wait(lock, [this] { return head != tail || stopLogging; });

            while (head != tail && batch.size() < BATCH_SIZE) {
                batch.push_back(buffer[tail]);
                tail = (tail + 1) % BUFFER_SIZE;
            }
            lock.unlock();

            if (!batch.empty()) {
                cout << "Processing " << batch.size() << " logs..." << endl;
                for (const string &log : batch) {
                    logFile << log << endl;
                    cout << "Written to file: " << log << endl;
                    currentLogSize += log.size();
                }
                logFile.flush();
                batch.clear();
            }

            if (currentLogSize >= MAX_LOG_SIZE) rotateLogs();
        }
    }

    void sendLogsToServer() {
        cout << "Network logging enabled. Connecting to server: " << serverIP << ":" << serverPort << endl;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            cerr << "Failed to create socket." << endl;
            return;
        }

        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
            cerr << "Failed to connect to server." << endl;
            close(sock);
            return;
        }

        vector<string> batch;
        while (!stopLogging || head != tail) {
            unique_lock<mutex> lock(bufferMutex);
            cv.wait(lock, [this] { return head != tail || stopLogging; });

            while (head != tail && batch.size() < BATCH_SIZE) {
                batch.push_back(buffer[tail]);
                tail = (tail + 1) % BUFFER_SIZE;
            }
            lock.unlock();

            if (!batch.empty()) {
                string batchMessage;
                for (const auto &log : batch) batchMessage += log + "\n";
                send(sock, batchMessage.c_str(), batchMessage.size(), 0);
                cout << "Sent " << batch.size() << " logs to server." << endl;
                batch.clear();
            }
        }
        close(sock);
    }

    void rotateLogs() {
        if (logFile.is_open()) logFile.close();
        cout << "Rotating logs..." << endl;

        for (int i = MAX_LOG_FILES - 1; i > 0; --i) {
            string oldName = "log_" + to_string(i) + ".txt";
            string newName = "log_" + to_string(i + 1) + ".txt";
            if (fs::exists(oldName)) {
                fs::rename(oldName, newName);
                cout << "Renamed " << oldName << " to " << newName << endl;
            }
        }

        if (fs::exists(baseFilename)) {
            fs::rename(baseFilename, "log_1.txt");
            cout << "Renamed " << baseFilename << " to log_1.txt" << endl;
        }

        logFile.open(baseFilename, ios::out | ios::trunc);
        if (!logFile.is_open()) cerr << "Error: Failed to create new log file: " << baseFilename << endl;

        currentLogSize = 0;
        cout << "Log rotation complete." << endl;
    }

    string formatLog(Level level, const string &message) {
        stringstream logEntry;
        logEntry << getTimestamp() << " [" << levelToString(level) << "] " << message;
        return logEntry.str();
    }

    string getTimestamp() {
        auto now = chrono::system_clock::now();
        auto timeT = chrono::system_clock::to_time_t(now);
        stringstream ss;
        ss << put_time(localtime(&timeT), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    string levelToString(Level level) {
        switch (level) {
            case INFO: return "INFO";
            case DEBUG: return "DEBUG";
            case ERROR: return "ERROR";
            default: return "UNKNOWN";
        }
    }
};

int main() {
    Logger logger("log.txt", Logger::DEBUG, Logger::PLAIN_TEXT, true, "127.0.0.1", 5000);

    logger.log(Logger::INFO, "This is an info message.");
    logger.log(Logger::DEBUG, "Debugging application.");
    logger.log(Logger::ERROR, "An error occurred!");

    this_thread::sleep_for(chrono::seconds(2));
    return 0;
}
