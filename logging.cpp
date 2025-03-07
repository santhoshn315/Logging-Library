#include <iostream>
#include <fstream>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <zlib.h> // For compression
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>

#include <fcntl.h> // Include this at the top for fsync()
namespace fs = std::filesystem;

using namespace std;

#define BUFFER_SIZE 1024         // Must be a power of 2
#define MAX_LOG_SIZE 1024 * 1024 // 1MB max log file size
#define MAX_LOG_FILES 5          // Keep last 5 log files
#define BATCH_SIZE 10            // Number of logs per network batch

class Logger
{
public:
    enum Level
    {
        INFO,
        DEBUG,
        ERROR
    };
    enum LogFormat
    {
        PLAIN_TEXT,
        JSON
    };

    Logger(const string &filename, Level minLogLevel = INFO, LogFormat format = PLAIN_TEXT, bool networkLogging = false, string serverIp = "127.0.0.1", int serverPort = 5000)
        : baseFilename(filename), stopLogging(false), head(0), tail(0), currentLogSize(0), minLevel(minLogLevel), logFormat(format), enableNetworkLogging(networkLogging), serverIP(serverIp), serverPort(serverPort)
    {
        rotateLogs();
        logFile.open(baseFilename, ios::out | ios::app);
        if (!logFile.is_open())
        {
            cerr << "Error: Failed to open log file: " << baseFilename << endl;
        }

        workerThread = thread(&Logger::processLogs, this);
        if (enableNetworkLogging)
        {
            networkThread = thread(&Logger::sendLogsToServer, this);
        }
    }

    ~Logger()
    {
        stopLogging = true;
        if (workerThread.joinable())
        {
            workerThread.join();
        }
        if (networkThread.joinable())
        {
            networkThread.join();
        }
        if (logFile.is_open())
        {
            cout << "Closing log file" << endl; // Debugging output
            logFile.flush();
            logFile.close();
        }
    }

    void log(Level level, const string &message)
    {
        if (level < minLevel)
            return; // Log filtering

        stringstream logEntry;
        logEntry << formatLog(level, message);

        size_t next = (head.load(memory_order_relaxed) + 1) % BUFFER_SIZE;
        if (next == tail.load(memory_order_acquire))
        {
            cerr << "Buffer full, dropping log: " << logEntry.str() << endl;
            return; // Drop log if buffer is full
        }

        buffer[head.load(memory_order_relaxed)] = logEntry.str();
        head.store(next, memory_order_release);

        cout << "Writing to log.txt: " << logEntry.str() << endl;

        // Write log entry to the file
        logFile << logEntry.str() << endl;
        logFile.flush(); // Ensure C++ stream flushes

        // Force OS to write the data immediately to disk
        int fd = open(baseFilename.c_str(), O_WRONLY | O_APPEND);
        if (fd != -1)
        {
            fsync(fd); // Force sync to disk
            close(fd);
        }
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

    void processLogs()
    {
        cout << "ProcessLogs thread started" << endl; // Debugging message

        while (!stopLogging || head.load(memory_order_acquire) != tail.load(memory_order_acquire))
        {
            if (head.load(memory_order_acquire) == tail.load(memory_order_acquire))
            {
                this_thread::yield();
                continue;
            }

            size_t index = tail.load(memory_order_relaxed);
            string logMessage = buffer[index];
            tail.store((index + 1) % BUFFER_SIZE, memory_order_release);

            cout << "Processing log: " << logMessage << endl;
            logFile << logMessage << endl;
            logFile.flush();

            // Force immediate write to disk
            int fd = open(baseFilename.c_str(), O_WRONLY | O_APPEND);
            if (fd != -1)
            {
                fsync(fd);
                close(fd);
            }
        }
    }

    void sendLogsToServer()
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return;

        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
        {
            close(sock);
            return;
        }

        vector<string> batch;
        while (!stopLogging || head.load(memory_order_acquire) != tail.load(memory_order_acquire))
        {
            if (head.load(memory_order_acquire) == tail.load(memory_order_acquire))
            {
                this_thread::yield();
                continue;
            }

            size_t index = tail.load(memory_order_relaxed);
            batch.push_back(buffer[index]);
            tail.store((index + 1) % BUFFER_SIZE, memory_order_release);

            if (batch.size() >= BATCH_SIZE)
            {
                string batchMessage;
                for (const auto &log : batch)
                {
                    batchMessage += log + "\n";
                }
                send(sock, batchMessage.c_str(), batchMessage.size(), 0);
                batch.clear();
            }
        }
        if (!batch.empty())
        {
            string batchMessage;
            for (const auto &log : batch)
            {
                batchMessage += log + "\n";
            }
            send(sock, batchMessage.c_str(), batchMessage.size(), 0);
        }
        close(sock);
    }

    void rotateLogs()
    {
        if (logFile.is_open())
        {
            logFile.close();
        }

        // Rotate old log files (log_4.txt -> log_5.txt, log_3.txt -> log_4.txt, ...)
        for (int i = MAX_LOG_FILES - 1; i > 0; --i)
        {
            string oldName = "log_" + to_string(i) + ".txt";
            string newName = "log_" + to_string(i + 1) + ".txt";
            if (fs::exists(oldName))
            {
                fs::rename(oldName, newName);
            }
        }

        // Rename the current log file to log_1.txt
        if (fs::exists(baseFilename))
        {
            fs::rename(baseFilename, "log_1.txt");
        }

        // Create a new empty log file (log.txt)
        logFile.open(baseFilename, ios::out | ios::trunc);
        if (!logFile.is_open())
        {
            cerr << "Error: Failed to create new log file: " << baseFilename << endl;
        }

        currentLogSize = 0;
    }

    string formatLog(Level level, const string &message)
    {
        stringstream logEntry;
        if (logFormat == JSON)
        {
            logEntry << "{"
                     << "\"timestamp\": \"" << getTimestamp() << "\", "
                     << "\"level\": \"" << levelToString(level) << "\", "
                     << "\"message\": \"" << message << "\""
                     << "}";
        }
        else
        {
            logEntry << getTimestamp() << " [" << levelToString(level) << "] " << message << "\n";
        }
        return logEntry.str();
    }

    string getTimestamp()
    {
        auto now = chrono::system_clock::now();
        auto timeT = chrono::system_clock::to_time_t(now);
        stringstream ss;
        ss << put_time(localtime(&timeT), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    string levelToString(Level level)
    {
        switch (level)
        {
        case INFO:
            return "INFO";
        case DEBUG:
            return "DEBUG";
        case ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
        }
    }
};

int main()
{
    Logger logger("logs.txt", Logger::DEBUG, Logger::JSON, true, "127.0.0.1", 5000);

    logger.log(Logger::INFO, "This is an info message.");
    logger.log(Logger::DEBUG, "Debugging application.");
    logger.log(Logger::ERROR, "An error occurred!");

    this_thread::sleep_for(chrono::seconds(2)); // Ensure logs are written
    return 0;
}
