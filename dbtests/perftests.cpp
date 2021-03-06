/** @file perftests.cpp.cpp : unit tests relating to performance

          The idea herein is tests that run fast and can be part of the normal CI suite.  So no tests herein that take
          a long time to run.  Obviously we need those too, but they will be separate.

          These tests use DBDirectClient; they are a bit white-boxish.
*/

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "../db/query.h"
#include "../db/db.h"
#include "../db/instance.h"
#include "../db/json.h"
#include "../db/lasterror.h"
#include "../db/update.h"
#include "../db/taskqueue.h"
#include "../util/timer.h"
#include "dbtests.h"
#include "../db/dur_stats.h"
#include "../util/checksum.h"
#include "../util/version.h"

using namespace bson;

namespace PerfTests {
    typedef DBDirectClient DBClientType;
    //typedef DBClientConnection DBClientType;

    class ClientBase {
    public:
        // NOTE: Not bothering to backup the old error record.
        ClientBase() {
            //_client.connect("localhost");
            mongo::lastError.reset( new LastError() );
        }
        virtual ~ClientBase() {
            //mongo::lastError.release();
        }
    protected:
        static void insert( const char *ns, BSONObj o ) {
            _client.insert( ns, o );
        }
        static void update( const char *ns, BSONObj q, BSONObj o, bool upsert = 0 ) {
            _client.update( ns, Query( q ), o, upsert );
        }
        static bool error() {
            return !_client.getPrevError().getField( "err" ).isNull();
        }
        DBClientBase &client() const { return _client; }
    private:
        static DBClientType _client;
    };
    DBClientType ClientBase::_client;

    // todo: use a couple threads. not a very good test yet.
    class TaskQueueTest {
        static int tot;
        struct V {
            int val;
            static void go(const V &v) { tot += v.val; }
        };
    public:
        void run() {
            tot = 0;
            TaskQueue<V> d;
            int x = 0;
            for( int i = 0; i < 100; i++ ) {
                if( i % 30 == 0 )
                    d.invoke();

                x += i;
                writelock lk;
                V v;
                v.val = i;
                d.defer(v);
            }
            d.invoke();
            assert( x == tot );
        }
    };
    int TaskQueueTest::tot;

    class B : public ClientBase {
        string _ns;
    protected:
        const char *ns() { return _ns.c_str(); }

        // anything you want to do before being timed
        virtual void prep() { }

        virtual void timed() = 0;

        // optional 2nd test phase to be timed separately
        // return name of it
        virtual const char * timed2() { return 0; }

        virtual void post() { }

        virtual string name() = 0;
        virtual unsigned long long expectation() { return 0; }
        virtual int expectationTimeMillis() { return -1; }

        // how long to run test.  0 is a sentinel which means just run the timed() method once and time it.
        virtual int howLongMillis() { return 5000; } 

        /* override if your test output doesn't need that */
        virtual bool showDurStats() { return true; }

        static DBClientConnection *conn;

    public:
        void say(unsigned long long n, int ms, string s) {
            unsigned long long rps = n*1000/ms;
            cout << "stats " << setw(33) << left << s << ' ' << setw(8) << rps << ' ' << right << setw(6) << ms << "ms ";
            if( showDurStats() )
                cout << dur::stats.curr->_asCSV();
            cout << endl;

            /* if you want recording of the timings, place the password for the perf database 
               in a "pstats.login" text file in the current directory for the test binary
            */
            const char *fn = "../../settings.py";
            static bool ok = true;
            if( ok && exists(fn) ) {
                try {
                    if( conn == 0 ) {
                        MemoryMappedFile f;
                        const char *p = (const char *) f.mapWithOptions(fn, MongoFile::READONLY);
                        string pwd;

                        {
                            const char *q = str::after(p, "pstatspassword=\"");
                            if( *q == 0 ) {
                                cout << "info perftests.cpp: no pstatspassword= in settings.py" << endl;
                                ok = false;
                            }
                            else {
                                pwd = str::before(q, '\"');
                            }
                        }

                        if( ok ) {
                            conn = new DBClientConnection(false, 0, 10);
                            string err;
                            if( conn->connect("mongo05.10gen.cust.cbici.net", err) ) { 
                                if( !conn->auth("perf", "perf", pwd, err) ) { 
                                    cout << "info: authentication with stats db failed: " << err << endl;
                                    assert(false);
                                }
                            }
                            else { 
                                cout << err << " (to log perfstats)" << endl;
                                ok = false;
                            }
                        }
                    }
                    if( conn && !conn->isFailed() ) { 
                        bob b;
                        b.append("host", getHostName());
                        b.appendTimeT("when", time(0));
                        b.append("test", s);
                        b.append("rps", (int) rps);
                        b.append("millis", ms);
                        b.appendBool("dur", cmdLine.dur);
                        if( showDurStats() && cmdLine.dur ) 
                            b.append("durStats", dur::stats.curr->_asObj());
                        {
                            bob inf;
                            inf.append("version", versionString);
                            if( sizeof(int*) == 4 ) inf.append("bits", 32);
    #if defined(_WIN32)
                            inf.append("os", "win");
    #endif
                            inf.append("git", gitVersion());
                            inf.append("boost", BOOST_VERSION);
                            b.append("info", inf.obj());
                        }

                        conn->insert("perf.pstats", b.obj());
                    }
                }
                catch(...) { 
                }
            }
        }
        void run() {
            _ns = string("perftest.") + name();
            client().dropCollection(ns());

            prep();

            int hlm = howLongMillis();

            dur::stats._intervalMicros = 0; // no auto rotate
            dur::stats.curr->reset();
            Timer t;
            unsigned long long n = 0;
            const unsigned Batch = 50;

            if( hlm == 0 ) { 
                // means just do once
                timed();
            }
            else {
                do {
                    unsigned i;
                    for( i = 0; i < Batch; i++ )
                        timed();
                    n += i;
                } while( t.millis() < hlm );
            }

            client().getLastError(); // block until all ops are finished
            int ms = t.millis();
            say(n, ms, name());

            int etm = expectationTimeMillis();
            if( etm > 0 ) { 
                if( ms > etm*2 ) { 
                    cout << "test  " << name() << " seems slow expected ~" << etm << "ms" << endl;
                }
            }
            else if( n < expectation() ) {
                cout << "test  " << name() << " seems slow n:" << n << " ops/sec but expect greater than:" << expectation() << endl;
            }

            {
                const char *test2name = timed2();
                if( test2name ) {
                    dur::stats.curr->reset();
                    Timer t;
                    unsigned long long n = 0;
                    while( 1 ) {
                        unsigned i;
                        for( i = 0; i < Batch; i++ )
                            timed2();
                        n += i;
                        if( t.millis() > hlm )
                            break;
                    }
                    int ms = t.millis();
                    say(n, ms, test2name);
                }
            }
        }
    };

    DBClientConnection *B::conn;

    unsigned dontOptimizeOutHopefully;

    class BSONIter : public B { 
    public:
        int n;
        bo b, sub;
        string name() { return "BSONIter"; }
        BSONIter() { 
            n = 0;
            bo sub = bob().appendTimeT("t", time(0)).appendBool("abool", true).appendBinData("somebin", 3, BinDataGeneral, "abc").appendNull("anullone").obj();
            b = BSON( "_id" << OID() << "x" << 3 << "yaaaaaa" << 3.00009 << "zz" << 1 << "q" << false << "obj" << sub << "zzzzzzz" << "a string a string" );
        }
        virtual bool showDurStats() { return false; }
        void timed() { 
            for( bo::iterator i = b.begin(); i.more(); )
                if( i.next().fieldName() )
                    n++;
            for( bo::iterator i = sub.begin(); i.more(); )
                if( i.next().fieldName() )
                    n++;
        }
    };

    // test thread local speed
    class TLS : public B {
    public:
        TLS() { }
        string name() { return "thread-local-storage"; }
        void timed() {
            if( &cc() )
                dontOptimizeOutHopefully++;
        }
        unsigned long long expectation() { return 1000000; }
        virtual bool showDurStats() { return false; }
    };

    // test speed of checksum method
    class ChecksumTest : public B {
    public:
        const unsigned sz;
        ChecksumTest() : sz(1024*1024*100+3)
        { }
        string name() { return "checksum"; }
        virtual int howLongMillis() { return 0; } 
        int expectationTimeMillis() { return 200; }
        virtual bool showDurStats() { return false; }

        void *p;

        void prep() { 
            {
                // the checksum code assumes 'standard' rollover on addition overflows. let's check that:
                unsigned long long x = 0xffffffffffffffffULL;
                ASSERT( x+2 == 1 );
            }

            p = malloc(sz);
            for (unsigned i = 0; i<sz; i++)
                ((char*)p)[i] = rand();
        }

        Checksum last;

        void timed() {
            for( int i = 0; i < 4; i++ ) { 
                Checksum c;
                c.gen(p, sz);
                ASSERT( i == 0 || c == last );
                last = c;
            }
        }
        void post() {
            {
                mongo::Checksum c;
                c.gen(p, sz-1);
                ASSERT( c != last );
                ((char *&)p)[0]++; // check same data, different order, doesn't give same checksum
                ((char *&)p)[1]--;
                c.gen(p, sz);
                ASSERT( c != last );
                ((char *&)p)[1]++; // check same data, different order, doesn't give same checksum (different longwords case)
                ((char *&)p)[8]--;
                c.gen(p, sz);
                ASSERT( c != last );
            }
            free(p);
        }
    };

    class InsertDup : public B {
        const BSONObj o;
    public:
        InsertDup() : o( BSON("_id" << 1) ) { } // dup keys
        string name() {
            return "insert-duplicate-_ids";
        }
        void prep() {
            client().insert( ns(), o );
        }
        void timed() {
            client().insert( ns(), o );
        }
        void post() {
            assert( client().count(ns()) == 1 );
        }
        unsigned long long expectation() { return 1000; }
    };

    class Insert1 : public InsertDup {
        const BSONObj x;
        OID oid;
        BSONObj query;
    public:
        Insert1() : x( BSON("x" << 99) ) { 
            oid.init();
            query = BSON("_id" << oid);
        }
        string name() { return "insert-simple"; }
        void timed() {
            client().insert( ns(), x );
        }
        const char * timed2() {
            client().findOne(ns(), query);
            return "findOne_by_id";
        }
        void post() {
            assert( client().count(ns()) > 100 );
        }
        unsigned long long expectation() { return 1000; }
    };

    class InsertBig : public InsertDup {
        BSONObj x;
        virtual int howLongMillis() {
            if( sizeof(void*) == 4 )
                return 1000;  // could exceed mmapping if run too long, as this function adds a lot fasta
            return 5000;
        }
    public:
        InsertBig() {
            char buf[200000];
            BSONObjBuilder b;
            b.append("x", 99);
            b.appendBinData("bin", 200000, (BinDataType) 129, buf);
            x = b.obj();
        }
        string name() { return "insert big"; }
        void timed() {
            client().insert( ns(), x );
        }
        unsigned long long expectation() { return 20; }
    };

    class InsertRandom : public B {
    public:
        string name() { return "random-inserts"; }
        void prep() {
            client().insert( ns(), BSONObj() );
            client().ensureIndex(ns(), BSON("x"<<1));
        }
        void timed() {
            int x = rand();
            BSONObj y = BSON("x" << x << "y" << rand() << "z" << 33);
            client().insert(ns(), y);
        }
        void post() {
        }
        unsigned long long expectation() { return 1000; }
    };

    /** upserts about 32k records and then keeps updating them
        2 indexes
    */
    class Update1 : public B {
    public:
        static int rand() {
            return std::rand() & 0x7fff;
        }
        virtual string name() { return "random-upserts"; }
        void prep() {
            client().insert( ns(), BSONObj() );
            client().ensureIndex(ns(), BSON("x"<<1));
        }
        void timed() {
            int x = rand();
            BSONObj q = BSON("x" << x);
            BSONObj y = BSON("x" << x << "y" << rand() << "z" << 33);
            client().update(ns(), q, y, /*upsert*/true);
        }

        const char * timed2() {
            static BSONObj I = BSON( "$inc" << BSON( "y" << 1 ) );

            // test some $inc's

            int x = rand();
            BSONObj q = BSON("x" << x);
            client().update(ns(), q, I);

            static string s = name()+" inc";
            return s.c_str();
        }

        void post() {
        }
        unsigned long long expectation() { return 1000; }
    };

    template <typename T>
    class MoreIndexes : public T {
    public:
        string name() { return T::name() + "-with-more-indexes"; }
        void prep() {
            T::prep();
            this->client().ensureIndex(this->ns(), BSON("y"<<1));
            this->client().ensureIndex(this->ns(), BSON("z"<<1));
        }
    };

    void t() {
        for( int i = 0; i < 20; i++ ) {
            sleepmillis(21);
            string fn = "/tmp/t1";
            MongoMMF f;
            unsigned long long len = 1 * 1024 * 1024;
            assert( f.create(fn, len, /*sequential*/rand()%2==0) );
            {
                char *p = (char *) f.getView();
                assert(p);
                // write something to the private view as a test
                strcpy(p, "hello");
            }
            if( cmdLine.dur ) {
                char *w = (char *) f.view_write();
                strcpy(w + 6, "world");
            }
            MongoFileFinder ff;
            ASSERT( ff.findByPath(fn) );
        }
    }

    class All : public Suite {
    public:
        All() : Suite( "perf" )
        {
        }
        ~All() { 
        }
        Result * run( const string& filter ) { 
            boost::thread a(t);
            Result * res = Suite::run(filter); 
            a.join();
            return res;
        }

        void setupTests() {
            cout
                << "stats test                              rps        time   "
                << dur::stats.curr->_CSVHeader() << endl;
            add< BSONIter >();
            add< ChecksumTest >();
            add< TaskQueueTest >();
            add< TLS >();
            add< InsertDup >();
            add< Insert1 >();
            add< InsertRandom >();
            add< MoreIndexes<InsertRandom> >();
            add< Update1 >();
            add< MoreIndexes<Update1> >();
            add< InsertBig >();
        }
    } myall;
}
