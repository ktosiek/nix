#include "shared.hh"
#include "local-store.hh"
#include "util.hh"
#include "serialise.hh"
#include "worker-protocol.hh"
#include "archive.hh"

#include <iostream>

using namespace nix;


static Path readStorePath(Source & from)
{
    Path path = readString(from);
    assertStorePath(path);
    return path;
}


static PathSet readStorePaths(Source & from)
{
    PathSet paths = readStringSet(from);
    for (PathSet::iterator i = paths.begin(); i != paths.end(); ++i)
        assertStorePath(*i);
    return paths;
}


static Sink * _to; /* !!! should make writeToStderr an object */
bool canSendStderr;


static void tunnelStderr(const unsigned char * buf, size_t count)
{
    writeFull(STDERR_FILENO, buf, count);
    if (canSendStderr) {
        try {
            writeInt(STDERR_NEXT, *_to);
            writeString(string((char *) buf, count), *_to);
        } catch (...) {
            /* Write failed; that means that the other side is
               gone. */
            canSendStderr = false;
            throw;
        }
    }
}


/* startWork() means that we're starting an operation for which we
   want to send out stderr to the client. */
static void startWork()
{
    canSendStderr = true;
}


/* stopWork() means that we're done; stop sending stderr to the
   client. */
static void stopWork()
{
    canSendStderr = false;
    writeInt(STDERR_LAST, *_to);
}


static void performOp(Source & from, Sink & to, unsigned int op)
{
    switch (op) {

#if 0        
    case wopQuit: {
        /* Close the database. */
        store.reset((StoreAPI *) 0);
        writeInt(1, to);
        break;
    }
#endif

    case wopIsValidPath: {
        Path path = readStorePath(from);
        writeInt(store->isValidPath(path), to);
        break;
    }

    case wopHasSubstitutes: {
        Path path = readStorePath(from);
        writeInt(store->hasSubstitutes(path), to);
        break;
    }

    case wopQueryPathHash: {
        Path path = readStorePath(from);
        writeString(printHash(store->queryPathHash(path)), to);
        break;
    }

    case wopQueryReferences:
    case wopQueryReferrers: {
        Path path = readStorePath(from);
        PathSet paths;
        if (op == wopQueryReferences)
            store->queryReferences(path, paths);
        else
            store->queryReferrers(path, paths);
        writeStringSet(paths, to);
        break;
    }

    case wopAddToStore: {
        /* !!! uberquick hack */
        string baseName = readString(from);
        bool fixed = readInt(from) == 1;
        bool recursive = readInt(from) == 1;
        string hashAlgo = readString(from);
        
        Path tmp = createTempDir();
        Path tmp2 = tmp + "/" + baseName;
        restorePath(tmp2, from);

        writeString(store->addToStore(tmp2, fixed, recursive, hashAlgo), to);
            
        deletePath(tmp);
        break;
    }

    case wopAddTextToStore: {
        string suffix = readString(from);
        string s = readString(from);
        PathSet refs = readStorePaths(from);
        writeString(store->addTextToStore(suffix, s, refs), to);
        break;
    }

    case wopBuildDerivations: {
        PathSet drvs = readStorePaths(from);
        startWork();
        store->buildDerivations(drvs);
        stopWork();
        writeInt(1, to);
        break;
    }

    case wopEnsurePath: {
        Path path = readStorePath(from);
        store->ensurePath(path);
        writeInt(1, to);
        break;
    }

    case wopAddTempRoot: {
        Path path = readStorePath(from);
        store->addTempRoot(path);
        writeInt(1, to);
        break;
    }

    case wopSyncWithGC: {
        store->syncWithGC();
        writeInt(1, to);
        break;
    }

    default:
        throw Error(format("invalid operation %1%") % op);
    }
}


static void processConnection(Source & from, Sink & to)
{
    store = boost::shared_ptr<StoreAPI>(new LocalStore(true));

    unsigned int magic = readInt(from);
    if (magic != WORKER_MAGIC_1) throw Error("protocol mismatch");

    writeInt(WORKER_MAGIC_2, to);

    debug("greeting exchanged");

    _to = &to;
    canSendStderr = false;
    writeToStderr = tunnelStderr;

    bool quit = false;

    unsigned int opCount = 0;
    
    do {
        WorkerOp op = (WorkerOp) readInt(from);

        opCount++;

        try {
            performOp(from, to, op);
        } catch (Error & e) {
            writeInt(STDERR_ERROR, *_to);
            writeString(e.msg(), to);
        }

    } while (!quit);

    printMsg(lvlError, format("%1% worker operations") % opCount);
}


void run(Strings args)
{
    bool slave = false;
    bool daemon = false;
    
    for (Strings::iterator i = args.begin(); i != args.end(); ) {
        string arg = *i++;
        if (arg == "--slave") slave = true;
        if (arg == "--daemon") daemon = true;
    }

    if (slave) {
        FdSource source(STDIN_FILENO);
        FdSink sink(STDOUT_FILENO);

        /* This prevents us from receiving signals from the terminal
           when we're running in setuid mode. */
        if (setsid() == -1)
            throw SysError(format("creating a new session"));

        processConnection(source, sink);
    }

    else if (daemon)
        throw Error("daemon mode not implemented");

    else
        throw Error("must be run in either --slave or --daemon mode");
}


#include "help.txt.hh"

void printHelp()
{
    std::cout << string((char *) helpText, sizeof helpText);
}


string programId = "nix-worker";