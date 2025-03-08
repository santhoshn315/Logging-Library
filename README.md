# 🚀 High-Performance Asynchronous Logging Library

*A C++ logging library optimized for speed, low-latency logging, and scalability.*

---

## 📌 Features  

- Logs asynchronously using a **Producer-Consumer model**, ensuring the main thread is never blocked.  
- Uses a **Lock-Free Ring Buffer** for high-speed log ingestion without mutex overhead.  
- Supports **batched writes** to both disk and network, reducing system call overhead and improving performance.  
- Runs on a **multi-threaded architecture**, with separate threads for log processing and network transmission.  
- Automatically **rotates log files** to manage storage efficiently.  
- Provides **custom log levels** like `INFO`, `DEBUG`, and `ERROR`.  
- Can optionally **send logs to a remote server** over TCP. 

---

## 📌 Prerequisites  
- C++17
