/*
 * Copyright 2015 Georgia Institute of Technology
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SUFFIX_TREE_HPP
#define SUFFIX_TREE_HPP

#include <vector>

#include <mxx/comm.hpp>
#include <mxx/timer.hpp>

#include <suffix_array.hpp>
#include <ansv.hpp>

// for posix sm
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

template <typename Q, typename Func>
std::vector<typename std::result_of<Func(Q)>::type> bulk_query(const std::vector<Q>& queries, Func f, const std::vector<size_t>& send_counts, const mxx::comm& comm) {
    // type of the query results
    typedef typename std::result_of<Func(Q)>::type T;
    mxx::section_timer t(std::cerr, comm);

    // get receive counts (needed as send counts for returning queries)
    std::vector<size_t> recv_counts = mxx::all2all(send_counts, comm);
    t.end_section("bulk_query: get recv_counts");

    // send all queries via all2all
    std::vector<Q> local_queries = mxx::all2allv(queries, send_counts, recv_counts, comm);
    t.end_section("bulk_query: all2all queries");

    // TODO: show load inbalance in queries and recv_counts?
    size_t recv_num = local_queries.size();
    std::pair<size_t, int> maxel = mxx::max_element(recv_num, comm);
    size_t total_queries = mxx::allreduce(queries.size(), comm);
    std::vector<size_t> recv_per_proc = mxx::gather(recv_num, 0, comm);
    if (comm.rank() == 0) {
        std::cerr << "Avg queries: " << total_queries * 1.0 / comm.size() << ", max queries on proc " << maxel.second << ": " << maxel.first << std::endl;
        std::cerr << "Inbalance factor: " << maxel.first * comm.size() * 1.0 / total_queries << "x" << std::endl;
        //std::cerr << "Queries received by each processor: " << recv_per_proc << std::endl;
    }

    // locally use query function for querying and save results
    std::vector<T> local_results(local_queries.size());
    for (size_t i = 0; i < local_queries.size(); ++i) {
        local_results[i] = f(local_queries[i]);
    }
    // now we can free the memory used for queries
    local_queries = std::vector<Q>();
    t.end_section("bulk_query: local query");

    // return all results, send_counts are the same as the recv_counts from the
    // previous all2all, and the other way around
    std::vector<T> results = mxx::all2allv(local_results, recv_counts, send_counts, comm);
    t.end_section("bulk_query: all2all query results");
    return results;
}

// global_adrs don't need to be sorted by address, but sorted by target processor
// TODO: generalize for where the global addresses/offsets are part of another data structure
template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& queries, const std::vector<size_t>& send_counts, const mxx::comm& comm) {

    // get local and global size
    size_t local_size = std::distance(local_begin, local_end);
    size_t prefix = mxx::exscan(local_size, comm);

    return bulk_query(queries,
                      [&local_begin, &prefix](size_t gladr) {
                            return *(local_begin + (gladr - prefix));
                      }, send_counts, comm);
}


template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& global_indexes, const mxx::comm& comm) {
    // get local and global size
    size_t local_size = std::distance(local_begin, local_end);
    size_t global_size = mxx::allreduce(local_size, comm);
    // get the block decomposition class and check that input is actuall block
    // decomposed
    // TODO: at one point, refactor this crap:
    mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());
    MXX_ASSERT(part.local_size() == local_size);

    // get send counts by linear scan (TODO: could get for free with the bucketing)
    // or cheaper with p*log(n)
    std::vector<size_t> send_counts(comm.size(), 0);
    int cur_p = 0;
    for (size_t i = 0; i < local_size; ++i) {
        int t = part.target_processor(global_indexes[i]);
        MXX_ASSERT(cur_p <= t);
        ++send_counts[t];
        cur_p = t;
    }

    return bulk_rma(local_begin, local_end, global_indexes, send_counts, comm);
}

template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma_mpiwin(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& global_indexes, const mxx::comm& comm) {
    using value_type = typename std::iterator_traits<InputIter>::value_type;

    // get local and global size
    size_t local_size = std::distance(local_begin, local_end);
    size_t global_size = mxx::allreduce(local_size, comm);
    mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());

    // create MPI_Win for input string, create character array for size of parents
    // and use RMA to request (read) all characters which are not `$`
    MPI_Win win;
    MPI_Win_create(&(*local_begin), local_size, sizeof(value_type), MPI_INFO_NULL, comm, &win);
    MPI_Win_fence(0, win);

    mxx::datatype dt = mxx::get_datatype<value_type>();

    // read characters here!
    std::vector<value_type> results(global_indexes.size());
    for (size_t i = 0; i < results.size(); ++i) {
        size_t offset = global_indexes[i];
        // read global index offset
        if (offset < global_size) {
            int proc = part.target_processor(offset);
            size_t proc_offset = offset - part.excl_prefix_size(proc);
            // request proc_offset from processor `proc` in window win
            MPI_Get(&results[i], 1, dt.type(), proc, proc_offset, 1, dt.type(), win);
        }
    }
    // fence to complete all requests
    //MPI_Win_fence(MPI_MODE_NOSTORE | MPI_MODE_NOPUT | MPI_MODE_NOSUCCEED, win);
    MPI_Win_fence(0, win);
    MPI_Win_free(&win);

    return results;
}

// TODO: separate further into Window and the global indexing stuff
//       ie: seprate into: global_array and backend implementation 
template <typename T>
class shmem_window_mpi {
// TODO: visablitiy
public:
    typedef T value_type;
    size_t global_size;
    size_t local_size;
    size_t prefix;
    const mxx::comm& comm;

    // private:
    MPI_Win win;
    value_type* charwin;
    value_type* shptr;

    template <typename Iterator>
    void init(Iterator local_begin, Iterator local_end) {
        mxx::section_timer t(std::cerr, comm);
        // get local and global size
        local_size = std::distance(local_begin, local_end);
        prefix = mxx::exscan(local_size, comm);
        global_size = mxx::allreduce(local_size, comm);
        mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());

        mxx::datatype dt = mxx::get_datatype<value_type>();

        // create MPI_Win for input string, create character array for size of parents
        // and use RMA to request (read) all characters which are not `$`
        MPI_Info info;
        MPI_Info_create(&info);
        MPI_Info_set(info, "alloc_shared_noncontig", "true");
        if (comm.rank() == 0) {
            MPI_Win_allocate_shared(global_size, sizeof(value_type), info, comm, &charwin, &win);
        } else {
            MPI_Win_allocate_shared(0, sizeof(value_type), info, comm, &charwin, &win);
        }
        t.end_section("alloc window");

        /* create table of pointers for shared memory access */
        //std::vector<CharT*> shptrs(comm.size());
        /*
           for (int i = 0; i < comm.size(); ++i) {
           MPI_Aint winsize;
           int windispls;
           MPI_Win_shared_query(win, i, &winsize, &windispls, &shptrs[i]);
           }
           */

        MPI_Aint winsize; int windispls;
        MPI_Win_shared_query(win, 0, &winsize, &windispls, &shptr);
        t.end_section("get ptrs via shared_query");

        /* copy string into shared memory window */
        memcpy(shptr+prefix, &(*local_begin), local_size*sizeof(value_type));
        t.end_section("copy input string into shared win");
        MPI_Win_sync(win);
        MPI_Win_fence(0, win);
        t.end_section("sync");
    }

    template <typename Iterator>
    shmem_window_mpi(Iterator local_begin, Iterator local_end, const mxx::comm& c) : comm(c) {
        init(local_begin, local_end);
    }

    inline T get(size_t gidx) const {
        return *(shptr + gidx);
    }

    virtual ~shmem_window_mpi() {
        MPI_Win_free(&win);
    }
};

template <typename T>
class shmem_window_posix {
// TODO: visablitiy
public:
    typedef T value_type;
    size_t global_size;
    size_t local_size;
    const mxx::comm& comm;

    // private:
    value_type* shptr;
    int sm_fd;

    template <typename Iterator>
    void init(Iterator local_begin, Iterator local_end) {
        mxx::section_timer t(std::cerr, comm);

        // get local and global size
        local_size = std::distance(local_begin, local_end);
        global_size = mxx::allreduce(local_size, comm);

        // create MPI_Win for input string, create character array for size of parents
        // and use RMA to request (read) all characters which are not `$`
        if (comm.rank() == 0) {
            sm_fd = shm_open("/my_shmem", O_CREAT | O_RDWR, 438);
            if (sm_fd == -1) {
                std::cerr << "couldn't open sm file" << std::endl;
                exit(EXIT_FAILURE);
            }
            if (ftruncate(sm_fd, sizeof(value_type)*global_size) == -1) {
                std::cerr << "couldn't truncate sm file" << std::endl;
                exit(EXIT_FAILURE);
            }
            shptr = (value_type*) mmap(NULL, sizeof(value_type)*global_size, PROT_READ | PROT_WRITE, MAP_SHARED, sm_fd, 0);
        }
        t.end_section("shm_open+alloc");

        // gather string into shmem page
        std::vector<size_t> recv_sizes = mxx::gather(local_size, 0, comm);
        mxx::gatherv(&(*local_begin), local_size, shptr, recv_sizes, 0, comm);
        t.end_section("gather string to master");

        // open shared memory pages for the string
        if (comm.rank() != 0) {
            sm_fd = shm_open("/my_shmem", O_RDONLY, 438);
            if (sm_fd == -1) {
                std::cerr << "couldn't open sm file on slave process" << std::endl;
                exit(EXIT_FAILURE);
            }
            shptr = (value_type*) mmap(NULL, sizeof(value_type)*global_size, PROT_READ, MAP_SHARED, sm_fd, 0);
        }
        comm.barrier();
        t.end_section("open shmem on rank != 0");
    }

    template <typename Iterator>
    shmem_window_posix(Iterator local_begin, Iterator local_end, const mxx::comm& c) : comm(c) {
        init(local_begin, local_end);
    }

    inline T get(size_t gidx) const {
        return *(shptr + gidx);
    }

    virtual ~shmem_window_posix() {
        // clean up shmem
        //munmap(shptr, sizeof(CharT)*global_size);
        if (comm.rank() == 0)
            shm_unlink("/my_shmem");
    }
};

template <typename T>
class shmem_window_posix_split {
// TODO: visablitiy
public:
    typedef T value_type;
    size_t global_size;
    size_t local_size;
    const mxx::comm& comm;

    // shmem files and mem-mapped ptrs
    std::vector<value_type*> shptrs;
    std::vector<size_t> group_data_sizes;
    std::vector<int> sm_fds;

    // group sizes (TODO: move into a separate hier-communicator object)
    mxx::comm subcomm;
    int num_groups;
    int group_size;
    int group_idx;

    template <typename Iterator>
    void init(Iterator local_begin, Iterator local_end) {
        mxx::section_timer t(std::cerr, comm);

        // get local and global size
        local_size = std::distance(local_begin, local_end);
        global_size = mxx::allreduce(local_size, comm);

        /* split communicator into groups */

        num_groups = 4; // split communicator into 4 subgroups
        if (num_groups > comm.size()) {
            num_groups = 1;
        }
        group_size = comm.size() / num_groups;
        // number of group
        group_idx = comm.rank() / group_size;
        // create subcommunicator (TODO: use hierarchical communicator/or 2D grid comm)
        subcomm = comm.split(group_idx);
        MXX_ASSERT(subcomm.rank() == comm.rank() % group_size);


        /* get data size for each group */
        shptrs.resize(num_groups);

        size_t group_data_size = mxx::allreduce(local_size, subcomm);
        // allgather group sizes
        mxx::comm span_comm = comm.split(subcomm.rank() == 0);
        if (subcomm.rank() == 0) {
            group_data_sizes = mxx::allgather(group_data_size, span_comm);
        }
        mxx::bcast(group_data_sizes, 0, subcomm);
        t.end_section("create groups");


        /* open shared memory on each group master */

        sm_fds.resize(num_groups);
        if (subcomm.rank() == 0) {
            char sh_name[20];
            sprintf(sh_name, "/my_shmem_%d", group_idx);

            sm_fds[group_idx] = shm_open(sh_name, O_CREAT | O_RDWR, 438);
            if (sm_fds[group_idx] == -1) {
                std::cerr << "couldn't open sm file" << std::endl;
                exit(EXIT_FAILURE);
            }
            if (ftruncate(sm_fds[group_idx], sizeof(value_type)*group_data_size) == -1) {
                std::cerr << "couldn't truncate sm file" << std::endl;
                exit(EXIT_FAILURE);
            }
            shptrs[group_idx] = (value_type*) mmap(NULL, sizeof(value_type)*group_data_size, PROT_READ | PROT_WRITE, MAP_SHARED, sm_fds[group_idx], 0);
        }
        t.end_section("shm_open+alloc");

        // gather string into shmem page
        std::vector<size_t> recv_sizes = mxx::gather(local_size, 0, subcomm);
        mxx::gatherv(&(*local_begin), local_size, shptrs[group_idx], recv_sizes, 0, subcomm);
        t.end_section("gather+convert string to group master");

        if (subcomm.rank() == 0) {
            char sh_name[20];
            sprintf(sh_name, "/my_shmem_%d", group_idx);
            // reopen shared mem in readonly mode
            munmap(shptrs[group_idx], sizeof(value_type)*group_data_size);
            close(sm_fds[group_idx]);
        }
        t.end_section("shm close on group-master");

        // open shared memory pages for the string
        for (int i = 0; i < num_groups; ++i) {
            char sh_name[20];
            sprintf(sh_name, "/my_shmem_%d", i);
            sm_fds[i] = shm_open(sh_name, O_RDONLY, 438);
            if (sm_fds[i] == -1) {
                std::cerr << "couldn't open sm file on slave process" << std::endl;
                exit(EXIT_FAILURE);
            }
            shptrs[i] = (value_type*) mmap(NULL, sizeof(value_type)*group_data_sizes[i], PROT_READ, MAP_SHARED, sm_fds[i], 0);
        }
        comm.barrier();
        t.end_section("open shmem on group masters");
    }

    template <typename Iterator>
    shmem_window_posix_split(Iterator local_begin, Iterator local_end, const mxx::comm& c) : comm(c) {
        init(local_begin, local_end);
    }

    inline T get(size_t gidx) const {
        for (int g = 0; g < num_groups; ++g) {
            if (gidx < group_data_sizes[g]) {
                return *(shptrs[g] + gidx);
            } else {
                gidx -= group_data_sizes[g];
            }
        }
        assert(false);
        return T();
    }

    virtual ~shmem_window_posix_split() {
        // clean up shmem
        //munmap(shptr, sizeof(CharT)*global_size);
        if (subcomm.rank() == 0) {
            char sh_name[20];
            sprintf(sh_name, "/my_shmem_%d", group_idx);
            shm_unlink(sh_name);
        }
    }
};

template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma_shm_mpi(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& global_indexes, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    using value_type = typename std::iterator_traits<InputIter>::value_type;

    shmem_window_mpi<value_type> win(local_begin, local_end, comm);

    // read characters here!
    std::vector<value_type> results(global_indexes.size());
    for (size_t i = 0; i < results.size(); ++i) {
        size_t offset = global_indexes[i];
        // read global index offset
        if (offset < win.global_size) {
            results[i] = win.get(offset);
        }
    }
    t.end_section("get all characters");

    comm.barrier();

    return results;
}

template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma_shm_posix(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& global_indexes, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    using value_type = typename std::iterator_traits<InputIter>::value_type;

    shmem_window_posix<value_type> win(local_begin, local_end, comm);

    // read characters here!
    std::vector<value_type> results(global_indexes.size());
    for (size_t i = 0; i < results.size(); ++i) {
        // read global index offset
        if (global_indexes[i] < win.global_size) {
            results[i] = win.get(global_indexes[i]);
        }
    }
    t.end_section("get all characters");

    comm.barrier();
    t.end_section(" barrier");

    return results;
}

// TODO: wrap the shared memory window thing into a class with deref opertor etc

template <typename InputIter>
std::vector<typename std::iterator_traits<InputIter>::value_type>
bulk_rma_shm_posix_split(InputIter local_begin, InputIter local_end,
         const std::vector<size_t>& global_indexes, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    using value_type = typename std::iterator_traits<InputIter>::value_type;

    shmem_window_posix_split<value_type> win(local_begin, local_end, comm);

    // read characters here!
    std::vector<value_type> results(global_indexes.size());
    for (size_t i = 0; i < results.size(); ++i) {
        // read global index offset
        if (global_indexes[i] < win.global_size) {
            results[i] = win.get(global_indexes[i]);
        }
    }
    t.end_section("get all characters");
    comm.barrier();
    t.end_section(" barrier");

    return results;
}


template <typename Func, typename InputIterator, typename index_t = std::size_t>
void for_each_parent(const suffix_array<InputIterator, index_t, true>& sa, Func func, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    // get input sizes
    size_t local_size = sa.local_SA.size();
    size_t global_size = mxx::allreduce(local_size, comm);
    size_t prefix = mxx::exscan(local_size, comm);
    // assert n >= p, or rather at least one element per process
    MXX_ASSERT(mxx::all_of(local_size >= 1, comm));
    // ansv of lcp!
    // TODO: use index_t instead of size_t
    std::vector<size_t> left_nsv;
    std::vector<size_t> right_nsv;
    std::vector<std::pair<index_t, size_t>> lr_mins;

    const size_t nonsv = std::numeric_limits<size_t>::max();
    t.end_section("pre ansv");

    // ANSV with furthest eq for left and smallest for right
    ansv<index_t, furthest_eq, nearest_sm, local_indexing>(sa.local_LCP, left_nsv, right_nsv, lr_mins, comm, nonsv);
    t.end_section("ansv");

    // each SA[i] lies between two LCP values
    // LCP[i] = lcp(S[SA[i-1]], S[SA[i]])
    // leaf nodes are the suffix array positions. Their parent is the either their left or their right
    // LCP, depending on which one is larger

    // get the first LCP value of the next processor
    index_t next_first_lcp = mxx::left_shift(sa.local_LCP[0], comm);
    for (size_t i = 0; i < local_size; ++i) {
        // for each suffix array position SA[i], we check the longest-common-prefix
        // with the neighboring suffixes SA[i-1] and SA[i+1]. Whichever one it
        // shares the larger common prefix with, is its sibling in the ST and
        // they share a parent at the depth given by the larger LCP value. The
        // index of the LCP that has that value will be the index of the parent
        // node.
        //
        // This means for every `i`, we need argmax_i {LCP[i], LCP[i+1]}, where
        // `i+1` might be on the next processor.
        //
        // If there are multiple leafs > 2 for an internal node, the parent
        // will be the index of the furthest equal element. We thus need
        // to use the NSV for determining the left parent.
        // If the right LCP is larger, then that one is the direct parent,
        // since there can't be any equal elements to the left (since the
        // right one was larger).

        // parent will be an index into LCP
        size_t parent = std::numeric_limits<size_t>::max();
        index_t lcp_val;

        // the globally first element has parent 1
        if (comm.rank() == 0 && i == 0) {
            // globally first leaf: SA[0]
            if (local_size > 1) {
                lcp_val = sa.local_LCP[1];
            } else {
                MXX_ASSERT(global_size > 1);
                lcp_val = next_first_lcp;
            }
            // -> parent = 1, since it is the common prefix between SA[0] and SA[1]
            // unless the lcp is 0, then this leaf is connected
            // directly to the root node (parent = 0)
            parent = lcp_val > 0 ? 1 : 0;
        } else {
            // To determine whether the left or right LCP is the parent,
            // we take the max of LCP[i]=lcp(SA[i-1],SA[i]) and LCP[i+1]=lcp(SA[i], SA[i+1])
            // There are two special cases to handle:
            // 1) locally last element: we need to use the first LCP value of the next processor
            //    in place of LCP[i+1]
            // 2) globally last element: parent is always the left furthest eq nsv
            if ((i == local_size-1
                 && (comm.rank() == comm.size() || sa.local_LCP[local_size-1] >= next_first_lcp))
                || (i < local_size-1 && sa.local_LCP[i] >= sa.local_LCP[i+1])) {
                // the parent is the left furthest eq or nearest sm
                size_t nsv;
                if (left_nsv[i] < local_size) {
                    nsv = prefix + left_nsv[i];
                    lcp_val = sa.local_LCP[left_nsv[i]];
                } else {
                    nsv = lr_mins[left_nsv[i] - local_size].second;
                    lcp_val = lr_mins[left_nsv[i] - local_size].first;
                }
                if (lcp_val == sa.local_LCP[i]) {
                    parent = nsv;
                } else {
                    parent = prefix + i;
                    lcp_val = sa.local_LCP[i];
                }
            } else {
                // SA[i] shares a longer prefix with its right neighbor SA[i+1]
                // they converge at internal node prefix+i+1
                parent = prefix + i + 1;
                if (i == local_size - 1)
                    lcp_val = next_first_lcp;
                else
                    lcp_val = sa.local_LCP[i+1];
            }
        }
        func(i, global_size + prefix + i, parent, lcp_val);
    }

    // get parents of internal nodes (via LCP)
    for (size_t i = 0; i < local_size; ++i) {
        size_t parent = std::numeric_limits<size_t>::max();
        index_t lcp_val;
        // for each LCP position, get ANSV left-furthest-eq and right-nearest-sm
        // and the max of the two is the parent
        // Special cases: first (LCP[0]) and globally last LCP
        if (comm.rank() == 0 && i == 0) {
            // this is the root node and it has no parent!
            continue;

        //} else if (comm.rank() == comm.size() - 1 && i == local_size - 1) {
            // globally last element (no right ansv)
            // this case is identical to the regular case, since for the right
            // most element, right_nsv[i] will be == nonsv
            // and as such is handled in the corresponding case below
        } else {
            if (sa.local_LCP[i] == 0) {
                // this is a dupliate of the root node which is located at
                // position 0 on processor 0
                continue;
            } else {
                // left NSV can't be non-existant because LCP[0] = 0
                assert(left_nsv[i] != nonsv);
                if (right_nsv[i] == nonsv) {
                    // use left one
                    size_t nsv;
                    if (left_nsv[i] < local_size) {
                        nsv = prefix + left_nsv[i];
                        lcp_val = sa.local_LCP[left_nsv[i]];
                    } else {
                        nsv = lr_mins[left_nsv[i] - local_size].second;
                        lcp_val = lr_mins[left_nsv[i] - local_size].first;
                    }
                    if (lcp_val == sa.local_LCP[i]) {
                        // duplicate node, don't add!
                        continue;
                    }
                    parent = nsv;
                } else {
                    // get left NSV index and value
                    size_t lnsv;
                    index_t left_lcp_val;
                    if (left_nsv[i] < local_size) {
                        lnsv = prefix + left_nsv[i];
                        left_lcp_val = sa.local_LCP[left_nsv[i]];
                    } else {
                        lnsv = lr_mins[left_nsv[i] - local_size].second;
                        left_lcp_val = lr_mins[left_nsv[i] - local_size].first;
                    }
                    // get right NSV index and value
                    size_t rnsv;
                    index_t right_lcp_val;
                    if (right_nsv[i] < local_size) {
                        rnsv = prefix + right_nsv[i];
                        right_lcp_val = sa.local_LCP[right_nsv[i]];
                    } else {
                        rnsv = lr_mins[right_nsv[i] - local_size].second;
                        right_lcp_val = lr_mins[right_nsv[i] - local_size].first;
                    }
                    // parent is the NSV for which LCP is larger.
                    // if same, use left furthest_eq
                    if (left_lcp_val >= right_lcp_val) {
                        if (left_lcp_val == sa.local_LCP[i]) {
                            // this is a duplicate node, and won't be added
                            continue;
                        }
                        parent = lnsv;
                        lcp_val = left_lcp_val;
                    } else {
                        parent = rnsv;
                        lcp_val = right_lcp_val;
                    }
                }
            }
        }
        func(i, prefix + i, parent, lcp_val);
    }
}

constexpr int edgechar_twophase_all2all = 1;
constexpr int edgechar_bulk_rma = 2;
constexpr int edgechar_mpi_osc_rma = 3;
constexpr int edgechar_rma_shared = 4;
constexpr int edgechar_posix_sm = 5;
constexpr int edgechar_posix_sm_split = 6;

//#if SHARED_MEM
constexpr int edgechar_default = edgechar_posix_sm_split;
/*
#else
constexpr int edgechar_default = edgechar_bulk_rma;
#endif
*/


/*
 * A rather in-efficient method, that requires two complete global shuffles
 * to request the edge character data
 *
 * This can be deleted eventually.
 */
template <typename InputIterator, typename index_t = std::size_t>
std::vector<size_t> construct_st_2phase(const suffix_array<InputIterator, index_t, true>& sa, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    // get input sizes
    size_t local_size = sa.local_SA.size();
    size_t global_size = mxx::allreduce(local_size, comm);
    size_t prefix = mxx::exscan(local_size, comm);
    // assert n >= p, or rather at least one element per process
    MXX_ASSERT(mxx::all_of(local_size >= 1, comm));

    std::vector<std::tuple<size_t, size_t, size_t>> parent_reqs;
    parent_reqs.reserve(2*local_size);
    // parent request where the character is the last `$`/`0` character
    // these don't have to be requested, but are locally fulfilled
    std::vector<std::tuple<size_t, size_t, size_t>> dollar_reqs;

    for_each_parent(sa, [&](size_t i, size_t gidx, size_t parent, size_t lcp_val) {
        if (sa.local_SA[i] + lcp_val >= global_size) {
            MXX_ASSERT(sa.local_SA[i] + lcp_val == global_size);
            dollar_reqs.emplace_back(parent, gidx, 0);
        } else {
            parent_reqs.emplace_back(parent, gidx, sa.local_SA[i] + lcp_val);
        }
    }, comm);
    t.end_section("locally calc parents");

    typedef typename std::iterator_traits<InputIterator>::value_type CharT;
    std::vector<CharT> edge_chars;

    // This is a slower method, because it sends all edges to the position
    // of the character first, and then back to the position of the parent.
    // Most parents are on the same processor as the child node, thus
    // this requires a lot more communication then necessary
    // 1) send tuples (parent, i, SA[i]+LCP[i]) to 3rd index)
    mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());
    // send all requests to the process on which the character for the
    // character request lies
    mxx::all2all_func(parent_reqs, [&part](const std::tuple<size_t,size_t,size_t>& t) {return part.target_processor(std::get<2>(t));}, comm);
    t.end_section("all2all_func: req characters");

    // replace string request with character from original string
    for (size_t i = 0; i < parent_reqs.size(); ++i) {
        size_t offset = std::get<2>(parent_reqs[i]);
        if (offset == global_size) {
            // the artificial last `$` character is mapped to 0
            std::get<2>(parent_reqs[i]) = 0;
        } else {
            // get character from that global string position
            std::get<2>(parent_reqs[i]) = sa.alphabet_mapping[static_cast<size_t>(*(sa.input_begin+(std::get<2>(parent_reqs[i])-prefix)))];
        }
    }
    // append the "dollar" requests
    parent_reqs.insert(parent_reqs.end(), dollar_reqs.begin(), dollar_reqs.end());
    dollar_reqs.clear(); dollar_reqs.shrink_to_fit();

    t.end_section("locally answer char queries");

    // 2) send tuples (parent, i, S[SA[i]+LCP[i]) to 1st index) [to parent]
    mxx::all2all_func(parent_reqs, [&part](const std::tuple<size_t,size_t,size_t>& t) {return part.target_processor(std::get<0>(t));}, comm);
    t.end_section("all2all_func: send to parent");

    // one internal node for each LCP entry, each internal node is sigma cells
    std::vector<size_t> internal_nodes((sa.sigma+1)*local_size);
    for (size_t i = 0; i < parent_reqs.size(); ++i) {
        size_t parent = std::get<0>(parent_reqs[i]);
        size_t node_idx = (parent - prefix)*(sa.sigma+1);
        uint16_t c = std::get<2>(parent_reqs[i]);
        MXX_ASSERT(0 <= c && c < sa.sigma+1);
        size_t cell_idx = node_idx + c;
        internal_nodes[cell_idx] = std::get<1>(parent_reqs[i]);
    }

    t.end_section("locally: create internal nodes");
}

template <typename InputIterator, typename index_t = std::size_t, int edgechar_method = edgechar_default>
std::vector<size_t> construct_suffix_tree(const suffix_array<InputIterator, index_t, true>& sa, const mxx::comm& comm) {
    mxx::section_timer t(std::cerr, comm);
    // get input sizes
    size_t local_size = sa.local_SA.size();
    size_t global_size = mxx::allreduce(local_size, comm);
    size_t prefix = mxx::exscan(local_size, comm);
    // assert n >= p, or rather at least one element per process
    MXX_ASSERT(mxx::all_of(local_size >= 1, comm));

    std::vector<std::tuple<size_t, size_t, size_t>> parent_reqs;
    parent_reqs.reserve(2*local_size);
    // parent request where the character is the last `$`/`0` character
    // these don't have to be requested, but are locally fulfilled
    std::vector<std::tuple<size_t, size_t, size_t>> dollar_reqs;
    std::vector<std::tuple<size_t, size_t, size_t>> remote_reqs;

    for_each_parent(sa, [&](size_t i, size_t gidx, size_t parent, size_t lcp_val) {
        if (prefix <= parent && parent < prefix + local_size) {
            parent_reqs.emplace_back(parent, gidx, sa.local_SA[i] + lcp_val);
        } else {
            remote_reqs.emplace_back(parent, gidx, sa.local_SA[i] + lcp_val);
        }
    }, comm);
    t.end_section("locally calc parents");

    // TODO: plus distinguish between dollar/parent req only for the first method
    typedef typename std::iterator_traits<InputIterator>::value_type CharT;
    std::vector<CharT> edge_chars;
    if (edgechar_method == edgechar_bulk_rma) {
        mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());
        // send those edges for which the parent lies on a remote processor
        typedef std::tuple<size_t, size_t, size_t> Tp;
        mxx::all2all_func(remote_reqs, [&part](const Tp& t) {return part.target_processor(std::get<0>(t));}, comm);
        parent_reqs.insert(parent_reqs.end(), remote_reqs.begin(), remote_reqs.end());
        remote_reqs = std::vector<Tp>();
        t.end_section("bulk_rma: send to parent");

        // only query for those with offset != global_size
        // bucket by target processor of the character request
        auto dollar_begin = std::partition(parent_reqs.begin(), parent_reqs.end(), [&global_size](const Tp& x){return std::get<2>(x) < global_size;});
        dollar_reqs = std::vector<Tp>(dollar_begin, parent_reqs.end());
        parent_reqs.resize(std::distance(parent_reqs.begin(), dollar_begin));
        t.end_section("bulk_rma: partition dollars");

        // bucket the String index by target processor (the character we need for this edge)
        // as a pre-step for the bulk_rma (which requires things to be bucketed by target processor)
        std::vector<size_t> send_counts = mxx::bucketing(parent_reqs, [&part](const std::tuple<size_t,size_t,size_t>& t) { return part.target_processor(std::get<2>(t));}, comm.size());
        t.end_section("bulk_rma: bucketing by char index");
        // create request address vector
        std::vector<size_t> global_indexes(parent_reqs.size());
        for (size_t i = 0; i < parent_reqs.size(); ++i) {
            global_indexes[i] = std::get<2>(parent_reqs[i]);
        }
        t.end_section("bulk_rma: create global_indexes");
        // use global bulk RMA for getting the corresponding characters
        edge_chars = bulk_rma(sa.input_begin, sa.input_end, global_indexes, send_counts, comm);
        t.end_section("bulk_rma: bulk_rma");
    } else {
        mxx::partition::block_decomposition_buffered<size_t> part(global_size, comm.size(), comm.rank());
        // send those edges for which the parent lies on a remote processor
        mxx::all2all_func(remote_reqs, [&part](const std::tuple<size_t,size_t,size_t>& t) {return part.target_processor(std::get<0>(t));}, comm);
        parent_reqs.insert(parent_reqs.end(), remote_reqs.begin(), remote_reqs.end());
        t.end_section("all2all_func: send to parent");

        std::vector<size_t> global_indexes(parent_reqs.size());
        for (size_t i = 0; i < parent_reqs.size(); ++i) {
            global_indexes[i] = std::get<2>(parent_reqs[i]);
        }
        t.end_section("create global_indexes");

        // TODO: bulk_rma_mpi only for non-dollar
        if (edgechar_method == edgechar_mpi_osc_rma) {
            edge_chars = bulk_rma_mpiwin(sa.input_begin, sa.input_end, global_indexes, comm);
        } else if (edgechar_method == edgechar_rma_shared) {
            edge_chars = bulk_rma_shm_mpi(sa.input_begin, sa.input_end, global_indexes, comm);
        } else if (edgechar_method == edgechar_posix_sm) {
            edge_chars = bulk_rma_shm_posix(sa.input_begin, sa.input_end, global_indexes, comm);
        } else if (edgechar_method == edgechar_posix_sm_split) {
            edge_chars = bulk_rma_shm_posix_split(sa.input_begin, sa.input_end, global_indexes, comm);
        }
        t.end_section("RMA read chars");
    }

    // TODO: (alternatives for full lookup table in each node:)
    // local hashing key=(node-idx, char), value=(child idx)
    //            or multimap key=(node-idx), value=(char, child idx)
    //            2nd enables iteration over children, but not direct lookup
    //            of specific child
    //            2nd no different than fixed std::vector<std::list>

    // one internal node for each LCP entry, each internal node is sigma cells
    std::vector<size_t> internal_nodes((sa.sigma+1)*local_size);
    for (size_t i = 0; i < parent_reqs.size(); ++i) {
        size_t parent = std::get<0>(parent_reqs[i]);
        size_t node_idx = (parent - prefix)*(sa.sigma+1);
        uint16_t c;
        CharT x = edge_chars[i];
        if (x == 0) {
            c = 0;
        } else {
            c = sa.alphabet_mapping[x];
        }
        MXX_ASSERT(0 <= c && c < sa.sigma+1);
        size_t cell_idx = node_idx + c;
        internal_nodes[cell_idx] = std::get<1>(parent_reqs[i]);
    }
    if (edgechar_method == edgechar_bulk_rma) {
        for (size_t i = 0; i < dollar_reqs.size(); ++i) {
            size_t parent = std::get<0>(dollar_reqs[i]);
            size_t node_idx = (parent - prefix)*(sa.sigma+1);
            internal_nodes[node_idx] = std::get<1>(dollar_reqs[i]);
        }
    }

    t.end_section("locally: create internal nodes");

    return internal_nodes;
}


#endif // SUFFIX_TREE_HPP
