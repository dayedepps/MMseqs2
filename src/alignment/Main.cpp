#include "Alignment.h"
#include "Parameters.h"
#include "Debug.h"
#include "Util.h"
#include "MMseqsMPI.h"

#ifdef OPENMP
#include <omp.h>
#endif


int align(int argc, const char **argv, const Command& command) {
    MMseqsMPI::init(argc, argv);

    Parameters& par = Parameters::getInstance();
    par.parseParameters(argc, argv, command, 4, true, 0, MMseqsParameter::COMMAND_ALIGN);

    Debug(Debug::INFO) << "Init data structures...\n";
    Alignment aln(par.db1, par.db1Index, par.db2, par.db2Index,
                  par.db3, par.db3Index, par.db4, par.db4Index, par);

    Debug(Debug::INFO) << "Calculation of Smith-Waterman alignments.\n";

#ifdef HAVE_MPI
    aln.run(MMseqsMPI::rank, MMseqsMPI::numProc, par.maxAccept, par.maxRejected);
#else
    aln.run(par.maxAccept, par.maxRejected);
#endif

    return EXIT_SUCCESS;
}


