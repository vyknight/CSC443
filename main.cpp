#include "kvdb.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstdio>

using namespace std;
using namespace std::chrono;

int main(int argc, char** argv) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <db_dir> <memtable_size> <total_data_MB>\n";
        return 1;
    }

    string dbdir = argv[1];
    int memcap = stoi(argv[2]);
    int total_data_MB = stoi(argv[3]);

    // Fixed-length key/value buffers: "k00000000"
    const int KEY_LEN  = 10; // including '\0'
    const int VAL_LEN  = 10;

    const int bytes_per_record = (KEY_LEN - 1) + (VAL_LEN - 1);  
    size_t total_bytes = total_data_MB * 1024ULL * 1024ULL;
    size_t total_records = total_bytes / bytes_per_record;

    cout << "Benchmark start: " << total_records 
         << " records (" << total_data_MB << " MB total)" << endl;

    KVDatabase db(dbdir, memcap);
    db.open();

    auto start = steady_clock::now();

    // ----------- PUT benchmark -----------
    for (size_t i = 0; i < total_records; ++i) {
        char keybuf[KEY_LEN];
        char valbuf[VAL_LEN];

        // fixed width: k00000001
        snprintf(keybuf, KEY_LEN, "k%08zu", i);
        snprintf(valbuf, VAL_LEN, "v%08zu", i);

        db.put(string(keybuf), string(valbuf));
    }

    db.close();
    auto end = steady_clock::now();

    double seconds = duration<double>(end - start).count();
    double throughput_MBps = total_data_MB / seconds;

    cout << "Time: " << seconds << " sec" << endl;
    cout << "Throughput: " << throughput_MBps << " MB/s" << endl;

    return 0;
}
