# Implementation of a Blockchain-Enabled Smart File System Using B-Tree Indexing

A high-performance, disk-resident storage engine developed in **C**. This project bridges the gap between traditional database efficiency and modern cryptographic security, creating an immutable data vault optimized for forensic transparency.

## 🚀 Core Features
* **B-Tree Indexing:** High-speed $O(\log n)$ search performance using 4KB disk-resident pages.
* **Blockchain Integrity:** SHA-256 hash-chain linking every data block; unauthorized tampering triggers an immediate "Breach Detected" alert.
* **Forensic GUI:** A professional Raylib-powered dashboard with a live **Hex-Viewer** to inspect raw binary disk data.
* **System Reliability:** Implements **LRU Caching** for memory optimization and **Shadow Paging** for atomic crash recovery.
* **Chaos Monkey:** Integrated "Tamper" feature to simulate real-time data corruption and test cryptographic audits.

## 🛠️ Technical Stack
* **Language:** C (C99)
* **Graphics:** Raylib 4.5+ (GUI & Visualization)
* **Cryptography:** OpenSSL (SHA-256 EVP API)
* **Environment:** Linux (Optimized for Kali Linux and Ubuntu)

## 📊 System Architecture
The engine organizes data into fixed **4KB binary pages**. The **B-Tree** manages structural location and offsets, while the **Blockchain** layer secures the content integrity. Any modification to the `.db` file—even a single bit change—breaks the cryptographic chain, securing the system against unauthorized edits.




## 📦 Installation & Build

### 1. Install Dependencies
Ensure you have the required development libraries installed:
```bash
sudo apt update
sudo apt install build-essential libssl-dev libraylib-dev
2. Build the Project
Use the provided Makefile to compile the source code:

Bash
make
3. Run the Dashboard
Launch the Forensic GUI:

Bash
./sentinelstorage
📝 Short Description
This project demonstrates advanced systems programming by combining the speed of database internals with the security of distributed ledger technology. It provides a transparent, unhackable file system that allows users to visualize raw data storage and integrity audits in real-time.
