# Implementation of a Blockchain-Enabled Smart File System Using B-Tree Indexing

A professional-grade, disk-resident storage engine built in **C**. This project bridges high-performance database indexing with cryptographic blockchain security to create an immutable data vault.

## 🚀 Core Features
* **B-Tree Indexing:** High-speed $O(\log n)$ search performance using disk-resident nodes.
* **Blockchain Integrity:** Every data block is linked via **SHA-256** hashes; any tampering triggers a "Breach Detected" alert.
* **Forensic GUI:** A Raylib-powered dashboard featuring a live **Hex-Viewer** to inspect raw disk bytes.
* **System Reliability:** Implements **LRU Caching** for speed and **Shadow Paging** for crash recovery.
* **Chaos Monkey:** Interactive "Tamper" button to simulate and detect real-time data corruption.

## 🛠️ Technical Stack
* **Language:** C (C99)
* **Graphics:** Raylib 4.5+
* **Cryptography:** OpenSSL (EVP API)
* **Environment:** Linux (Optimized for Kali/Ubuntu)

## 📦 Installation & Build
1. **Install Dependencies:**
   ```bash
   sudo apt update
   sudo apt install libssl-dev libraylib-dev
   Compile:

Bash
make
Run:

Bash
./sentinelstorage

Gemini said
Looks like you're ready to set up your GitHub README.md! 🚀

Since you have all your files uploaded (I see btree.c, blockchain.c, gui.c, etc. in your repository), you need a professional README to show off your work. Copy and paste the code below into that edit box on your screen.

Markdown
# Implementation of a Blockchain-Enabled Smart File System Using B-Tree Indexing

A professional-grade, disk-resident storage engine built in **C**. This project bridges high-performance database indexing with cryptographic blockchain security to create an immutable data vault.

## 🚀 Core Features
* **B-Tree Indexing:** High-speed $O(\log n)$ search performance using disk-resident nodes.
* **Blockchain Integrity:** Every data block is linked via **SHA-256** hashes; any tampering triggers a "Breach Detected" alert.
* **Forensic GUI:** A Raylib-powered dashboard featuring a live **Hex-Viewer** to inspect raw disk bytes.
* **System Reliability:** Implements **LRU Caching** for speed and **Shadow Paging** for crash recovery.
* **Chaos Monkey:** Interactive "Tamper" button to simulate and detect real-time data corruption.

## 🛠️ Technical Stack
* **Language:** C (C99)
* **Graphics:** Raylib 4.5+
* **Cryptography:** OpenSSL (EVP API)
* **Environment:** Linux (Optimized for Kali/Ubuntu)

## 📦 Installation & Build
1. **Install Dependencies:**
   ```bash
   sudo apt update
   sudo apt install libssl-dev libraylib-dev
Compile:

Bash
make
Run:

Bash
./sentinelstorage
📊 System Architecture
The system organizes data into fixed 4KB pages. The B-Tree manages the location (offsets), while the Blockchain secures the content (integrity). If a single bit is changed in the .db file via a Hex Editor, the next audit will fail, securing the system against unauthorized edits.


---

### Why this is good for your GitHub:
* **Visual Structure:** It uses bold headers and icons to make it readable.
* **Professional Tone:** It emphasizes the "Forensic" and "Security" aspects of your project.
* **Clear Instructions:** Anyone visiting your profile can now clone and run your project with the provided commands.


**Once you paste this, click "Commit changes..." at the top right of your image to save 
