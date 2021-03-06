#include "result2stats.h"

#include "Alignment.h"
#include "Debug.h"
#include "Util.h"
#include "FileUtil.h"
#include "itoa.h"

#include <cstdlib>
#include <cmath>

#ifdef OPENMP
#include <omp.h>
#endif

enum {
    STAT_LINECOUNT,
    STAT_MEAN,
    STAT_SUM,
    STAT_DOOLITTLE,
    STAT_CHARGES,
    STAT_SEQLEN,
    STAT_STRLEN,
    STAT_FIRSTLINE,
    STAT_UNKNOWN
};

int MapStatString(const std::string &str) {
    int stat;
    if (str == "linecount") {
        stat = STAT_LINECOUNT;
    } else if (str == "mean") {
        stat = STAT_MEAN;
    } else if (str == "sum") {
        stat = STAT_SUM;
    } else if (str == "doolittle") {
        stat = STAT_DOOLITTLE;
    } else if (str == "charges") {
        stat = STAT_CHARGES;
    } else if (str == "seqlen") {
        stat = STAT_SEQLEN;
    } else if (str == "strlen") {
        stat = STAT_STRLEN;
    } else if (str == "firstline") {
        stat = STAT_FIRSTLINE;
    } else {
        stat = STAT_UNKNOWN;
    }

    return stat;
}

StatsComputer::StatsComputer(const Parameters &par)
        : stat(MapStatString(par.stat)),
          queryDb(par.db1), queryDbIndex(par.db1Index),
          targetDb(par.db2), targetDbIndex(par.db2Index) {
    resultReader = new DBReader<unsigned int>(par.db3.c_str(), par.db3Index.c_str());
    resultReader->open(DBReader<unsigned int>::LINEAR_ACCCESS);

    statWriter = new DBWriter(par.db4.c_str(), par.db4Index.c_str(), (unsigned int) par.threads, DBWriter::BINARY_MODE);
    statWriter->open();
}

float doolittle(const char*);
float charges(const char*);
size_t seqlen(const char* sequence) {
    const char* p = sequence;
    size_t length = 0;
    while (p != NULL) {
        if ((*p >= 'A' && *p < 'Z') || (*p >= 'a' && *p < 'z') || *p == '*') {
            length++;
        } else {
            break;
        }

        p++;
    }
    return length;
}
std::string firstline(const char*);

int StatsComputer::run() {
    switch (stat) {
        case STAT_LINECOUNT:
            return countNumberOfLines();
        case STAT_MEAN:
            return meanValue();
        case STAT_SUM:
            return sumValue();
        case STAT_DOOLITTLE:
            return sequenceWise<float>(&doolittle);
        case STAT_CHARGES:
            return sequenceWise<float>(&charges);
        case STAT_SEQLEN:
            return sequenceWise<size_t>(&seqlen);
        case STAT_STRLEN:
            return sequenceWise<size_t>(&std::strlen);
        case STAT_FIRSTLINE:
            return sequenceWise<std::string>(&firstline, true);
        //case STAT_COMPOSITION:
        //    return sequenceWise(&statsComputer::composition);
        case STAT_UNKNOWN:
        default:
            Debug(Debug::ERROR) << "Unrecognized statistic: " << stat << "\n";
            EXIT(EXIT_FAILURE);
    }
}

StatsComputer::~StatsComputer() {
    statWriter->close();
    resultReader->close();
    delete statWriter;
    delete resultReader;
}

int StatsComputer::countNumberOfLines() {
#pragma omp parallel for schedule(static)
    for (size_t id = 0; id < resultReader->getSize(); id++) {
        Debug::printProgress(id);
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif

        unsigned int lineCount(0);
        std::string lineCountString;

        char *results = resultReader->getData(id);
        while (*results != '\0') {
            if (*results == '\n') {
                lineCount++;
            }
            results++;
        }

        lineCountString = SSTR(lineCount) + "\n";

        statWriter->writeData(lineCountString.c_str(), lineCountString.length(), resultReader->getDbKey(id), thread_idx);
    }
    return 0;
}


int StatsComputer::meanValue() {
#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif
#pragma omp for schedule(static)
        for (size_t id = 0; id < resultReader->getSize(); id++) {
            Debug::printProgress(id);
            char *results = resultReader->getData(id);

            double meanVal = 0.0;
            size_t nbSeq = 0;
            while (*results != '\0') {
                char *rest;
                errno = 0;
                double value = strtod(results, &rest);
                if (rest == results || errno != 0) {
                    Debug(Debug::WARNING) << "Invalid value in entry " << id << "!\n";
                    continue;
                }

                meanVal += value;
                nbSeq++;
                results = Util::skipLine(results);
            }

            std::string meanValString = SSTR(meanVal / std::max(static_cast<size_t>(1), nbSeq));
            meanValString.append("\n");
            statWriter->writeData(meanValString.c_str(), meanValString.length(), resultReader->getDbKey(id), thread_idx);
        }
    };
    return 0;
}

int StatsComputer::sumValue() {
#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif
        char buffer[1024];
#pragma omp for schedule(static)
        for (size_t id = 0; id < resultReader->getSize(); id++) {
            Debug::printProgress(id);
            char *results = resultReader->getData(id);

            size_t sum = 0;
            while (*results != '\0') {
                char *rest;
                errno = 0;
                size_t value = strtoull(results, &rest, 10);
                if (rest == results || errno != 0) {
                    Debug(Debug::WARNING) << "Invalid value in entry " << id << "!\n";
                    continue;
                }

                sum += value;
                results = Util::skipLine(results);
            }

            std::string meanValString = SSTR(sum) + "\n";
            char *end = Itoa::u64toa_sse2(sum, buffer);
            *(end - 1) = '\n';

            statWriter->writeData(buffer, end - buffer, resultReader->getDbKey(id), thread_idx);
        }
    };
    return 0;
}

float averageValueOnAminoAcids(const std::unordered_map<char, float> &values, const char *seq) {
    const char *seqPointer = seq;
    float ret = values.at('0') + values.at('1'); // C ter and N ter values
    std::unordered_map<char, float>::const_iterator k;

    while (*seqPointer != '\0' && *seqPointer != '\n') {
        if ((k = values.find(tolower(*seqPointer))) != values.end()) {
            ret += k->second;
        }

        seqPointer++;
    }

    size_t seqLen = seqPointer - seq;
    return ret / std::max(static_cast<size_t>(1), seqLen);
}

class Doolittle {
public:
    std::unordered_map<char, float> values;

    Doolittle() {
        values['a'] = 6.3;
        values['r'] = 0.0;
        values['n'] = 1.0;
        values['d'] = 1.0;
        values['c'] = 7.0;
        values['q'] = 1.0;
        values['e'] = 1.0;
        values['g'] = 4.1;
        values['h'] = 1.3;
        values['i'] = 9.0;
        values['l'] = 5.2;
        values['k'] = 0.6;
        values['m'] = 6.4;
        values['f'] = 7.2;
        values['p'] = 2.9;
        values['s'] = 3.6;
        values['t'] = 3.8;
        values['w'] = 3.6;
        values['y'] = 3.2;
        values['v'] = 8.7;
        values['x'] = 0.0;
        values['0'] = 0.0; // N-ter
        values['1'] = 0.0; // C-ter
    }
};

float doolittle(const char *seq) {
    static Doolittle doolitle;
    return averageValueOnAminoAcids(doolitle.values, seq);
}

class Charges {
public:
    std::unordered_map<char, float> values;

    Charges() {
        const float pH = 7.0;

        std::unordered_map<char, float> pKs, chargeSign;

        // pKs values:
        // Bjellqvist Dawson EMBOSS Lehninger Murray Rodwell Sillero Solomon Stryer
        pKs['c'] = 9.00;//    8.3    8.5      8.18   8.33    8.33     9.0     8.3    8.5
        pKs['d'] = 4.05;//    3.9    3.9      3.65   3.68    3.86     4.0     3.9    4.4
        pKs['e'] = 4.45;//    4.3    4.1      4.25   4.25    4.25     4.5     4.3    4.4
        pKs['h'] = 5.98;//    6.0    6.5      6.00   6.00    6.00     6.4     6.0    6.5
        pKs['k'] = 10.00;//   10.5   10.8     10.53  11.50   11.50    10.4    10.5   10.0
        pKs['r'] = 12.00;//   12.0   12.5     12.48  11.50   11.50    12.0    12.5   12.0
        pKs['y'] = 10.00;//   10.1   10.1     10.07  10.07   10.70    10.0    10.1   10.0
        pKs['1'] = 3.55;//    3.2    3.6      2.34   2.15    3.10     3.2     3.2    3.2 // C ter
        pKs['0'] = 7.50;//    8.2    8.6      9.69   9.52    8.00     8.2     8.2    8.2 // N ter

        chargeSign['c'] = -1.0f;
        chargeSign['d'] = -1.0f;
        chargeSign['e'] = -1.0f;
        chargeSign['y'] = -1.0f;
        chargeSign['h'] = 1.0f;
        chargeSign['k'] = 1.0f;
        chargeSign['r'] = 1.0f;
        chargeSign['1'] = -1.0f; // C ter
        chargeSign['0'] = 1.0f; // N ter

        for (std::unordered_map<char, float>::iterator k = pKs.begin(); k != pKs.end(); k++) {
            values[k->first] = chargeSign[k->first] / (1 + pow(10, (chargeSign[k->first] * (pH - pKs[k->first]))));
        }
    }
};


float charges(const char *seq) {
    static Charges charges;
    return averageValueOnAminoAcids(charges.values, seq);
}

std::string firstline(const char *seq) {
    std::stringstream ss(seq);
    std::string line;
    std::getline(ss, line);
    return line;
}

template<typename T>
int StatsComputer::sequenceWise(typename PerSequence<T>::type call, bool onlyResultDb) {

    bool sameQTDB = false;
    DBReader<unsigned int> *queryReader = NULL, *targetReader = NULL;
    if (!onlyResultDb) {
        queryReader = new DBReader<unsigned int>(queryDb.c_str(), queryDbIndex.c_str());
        queryReader->open(DBReader<unsigned int>::NOSORT);

        sameQTDB = (queryDb.compare(targetDb) == 0);
        if (sameQTDB) {
            targetReader = queryReader;
        } else {
            targetReader = new DBReader<unsigned int>(targetDb.c_str(), targetDbIndex.c_str());
            targetReader->open(DBReader<unsigned int>::NOSORT);
        }
    }

#pragma omp parallel for schedule(static)
    for (size_t id = 0; id < resultReader->getSize(); id++) {
        Debug::printProgress(id);
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = (unsigned int) omp_get_thread_num();
#endif
        char dbKey[255 + 1];

        std::ostringstream ss;
        char *results = resultReader->getData(id);

        if (onlyResultDb) {
            T stat = (*call)(results);
            ss << SSTR(stat) << '\n';

        } else {
            // for every hit
            int cnt = 0;
            while (*results != '\0') {
                Util::parseKey(results, dbKey);
                char *rest;

                const unsigned int key = (unsigned int) strtoul(dbKey, &rest, 10);
                if ((rest != dbKey && *rest != '\0') || errno == ERANGE) {
                    Debug(Debug::WARNING) << "Invalid key in entry " << id << "!\n";
                    continue;
                }

                const char *dbSeqData;
                if (cnt == 0) {
                    const size_t edgeId = queryReader->getId(key);
                    dbSeqData = queryReader->getData(edgeId);
                } else {
                    const size_t edgeId = targetReader->getId(key);
                    dbSeqData = targetReader->getData(edgeId);
                }

                T stat = (*call)(dbSeqData);
                ss << SSTR(stat) << '\n';

                results = Util::skipLine(results);
                cnt++;
            }
        }

        std::string statString = ss.str();
        statWriter->writeData(statString.c_str(), statString.length(), resultReader->getDbKey(id), thread_idx);
    }

    if(!onlyResultDb) {
        queryReader->close();
        delete queryReader;
        if (!sameQTDB) {
            targetReader->close();
            delete targetReader;
        }
    }
    return 0;
}

int result2stats(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 4);

#ifdef OPENMP
    omp_set_num_threads(par.threads);
#endif

    StatsComputer computeStats(par);
    return computeStats.run();
}
