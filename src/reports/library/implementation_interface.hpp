#pragma once

#include <tuple>
#include <iostream>
#include <algorithm>
#include <numeric>
#include <hdf5.h>

#if defined(HAVE_MPI)
#include <mpi.h>
#endif

namespace detail {

    template <class TImpl>
    struct Implementation {
        inline static int init() {
            return TImpl::init();
        }
        inline static void init_comm(int reports) {
            TImpl::init_comm(reports);
        }
        inline static void close() {
            TImpl::close();
        }
        inline static std::tuple<hid_t, hid_t> prepare_write(hid_t plist_id) {
            return TImpl::prepare_write(plist_id);
        }
        inline static hsize_t get_offset(hsize_t value) {
            return TImpl::get_offset(value);
        }
        inline static hsize_t get_global_dims(hsize_t value) {
            return TImpl::get_global_dims(value);
        }
        inline static void sort_spikes(std::vector<double>& spikevec_time, std::vector<int>& spikevec_gid) {
            TImpl::sort_spikes(spikevec_time, spikevec_gid);
        }
    };

#if defined(HAVE_MPI)



    struct ParallelImplementation {

        inline static void init_comm(int reports) {
            int global_rank, global_size;
            MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);
            MPI_Comm_size(MPI_COMM_WORLD, &global_size);
            std::cout << "++++++Num reports for rank " << global_rank << " is " << reports<< std::endl;

            // Create a second communicator
            MPI_Comm_split(MPI_COMM_WORLD, reports == 0, 0, &ReportingLib::m_allCells);

        };

        inline static int init() {
            int global_rank, global_size;
            MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);
            MPI_Comm_size(MPI_COMM_WORLD, &global_size);

            int cell_rank, cell_size;
            MPI_Comm_rank(ReportingLib::m_allCells, &cell_rank);
            MPI_Comm_size(ReportingLib::m_allCells, &cell_size);

            printf("+++++++++WORLD RANK/SIZE: %d/%d \t ROW RANK/SIZE: %d/%d\n", global_rank, global_size, cell_rank, cell_size);
            return cell_rank;
        };

        inline static void close(){};
        inline static std::tuple<hid_t, hid_t> prepare_write(hid_t plist_id) {
            // Enable MPI access
            MPI_Info info = MPI_INFO_NULL;
            H5Pset_fapl_mpio(plist_id, ReportingLib::m_allCells, info);

            // Initialize independent/collective lists
            hid_t collective_list = H5Pcreate(H5P_DATASET_XFER);
            hid_t independent_list = H5Pcreate(H5P_DATASET_XFER);
            H5Pset_dxpl_mpio(collective_list, H5FD_MPIO_COLLECTIVE);
            H5Pset_dxpl_mpio(independent_list, H5FD_MPIO_INDEPENDENT);

            return std::make_tuple(collective_list, independent_list);
        };
        inline static hsize_t get_offset(hsize_t value) {
            hsize_t offset = 0;
            MPI_Scan(&value, &offset, 1, MPI_UNSIGNED_LONG, MPI_SUM, ReportingLib::m_allCells);
            offset -= value;
            return offset;
        };
        inline static hsize_t get_global_dims(hsize_t value) {
            hsize_t global_dims = value;
            MPI_Allreduce(&value, &global_dims, 1, MPI_UNSIGNED_LONG, MPI_SUM, ReportingLib::m_allCells);
            return global_dims;
        };
        inline static void sort_spikes(std::vector<double>& spikevec_time, std::vector<int>& spikevec_gid) {
            int numprocs;
            MPI_Comm_size(ReportingLib::m_allCells, &numprocs);
            double lmin_time = *(std::min_element(spikevec_time.begin(), spikevec_time.end()));
            double lmax_time = *(std::max_element(spikevec_time.begin(), spikevec_time.end()));
            double min_time = nrnmpi_dbl_allmin(lmin_time, numprocs);
            double max_time = nrnmpi_dbl_allmax(lmax_time, numprocs);

            std::vector<double> inTime = spikevec_time;
            std::vector<int> inGid = spikevec_gid;
            local_spikevec_sort(inTime, inGid, spikevec_time, spikevec_gid);

            // allocate send and receive counts and displacements for MPI_Alltoallv
            std::vector<int> snd_cnts(numprocs);
            std::vector<int> rcv_cnts(numprocs);
            std::vector<int> snd_dsps(numprocs);
            std::vector<int> rcv_dsps(numprocs);

            double bin_t = (max_time - min_time) / numprocs;

            bin_t = bin_t ? bin_t : 1;
            // first find number of spikes in each time window
            for (const auto& st : spikevec_time) {
                int idx = (int)(st - min_time) / bin_t;
                snd_cnts[idx]++;
            }

            // now let each rank know how many spikes they will receive
            // and get in turn all the buffer sizes to receive
            nrnmpi_int_alltoall(&snd_cnts[0], &rcv_cnts[0], 1);
            for (int i = 1; i < numprocs; i++) {
                rcv_dsps[i] = rcv_dsps[i - 1] + rcv_cnts[i - 1];
            }

            for (int i = 1; i < numprocs; i++) {
                snd_dsps[i] = snd_dsps[i - 1] + snd_cnts[i - 1];
            }

            std::size_t new_sz = 0;
            for (const auto& r : rcv_cnts) {
                new_sz += r;
            }

            // prepare new sorted vectors
            std::vector<double> svt_buf(new_sz, 0.0);
            std::vector<int> svg_buf(new_sz, 0);

            // now exchange data
            nrnmpi_dbl_alltoallv(spikevec_time.data(), &snd_cnts[0], &snd_dsps[0], svt_buf.data(),
                                 &rcv_cnts[0], &rcv_dsps[0]);
            nrnmpi_int_alltoallv(spikevec_gid.data(), &snd_cnts[0], &snd_dsps[0], svg_buf.data(),
                                 &rcv_cnts[0], &rcv_dsps[0]);

            local_spikevec_sort(svt_buf, svg_buf, spikevec_time, spikevec_gid);

        };

        // Aux functions for sort_spikes
        static double nrnmpi_dbl_allmin(double x, int numprocs) {
            double result;
            if (numprocs < 2) {
                return x;
            }
            MPI_Allreduce(&x, &result, 1, MPI_DOUBLE, MPI_MIN, ReportingLib::m_allCells);
            return result;
        }

        static double nrnmpi_dbl_allmax(double x, int numprocs) {
            double result;
            if (numprocs < 2) {
                return x;
            }
            MPI_Allreduce(&x, &result, 1, MPI_DOUBLE, MPI_MAX, ReportingLib::m_allCells);
            return result;
        }

        static void nrnmpi_int_alltoall(int* s, int* r, int n) {
            MPI_Alltoall(s, n, MPI_INT, r, n, MPI_INT, ReportingLib::m_allCells);
        }

        static void nrnmpi_dbl_alltoallv(double* s, int* scnt,int* sdispl,
                                                double* r, int* rcnt, int* rdispl) {
            MPI_Alltoallv(s, scnt, sdispl, MPI_DOUBLE, r, rcnt, rdispl, MPI_DOUBLE, ReportingLib::m_allCells);
        }

        static void nrnmpi_int_alltoallv(int* s, int* scnt, int* sdispl, int* r, int* rcnt, int* rdispl) {
            MPI_Alltoallv(s, scnt, sdispl, MPI_INT, r, rcnt, rdispl, MPI_INT, ReportingLib::m_allCells);
        }

        static void local_spikevec_sort(std::vector<double>& isvect,
                                               std::vector<int>& isvecg,
                                               std::vector<double>& osvect,
                                               std::vector<int>& osvecg) {
            osvect.resize(isvect.size());
            osvecg.resize(isvecg.size());
            // first build a permutation vector
            std::vector<std::size_t> perm(isvect.size());
            std::iota(perm.begin(), perm.end(), 0);
            // sort by gid (second predicate first)
            std::stable_sort(perm.begin(), perm.end(),
                             [&](std::size_t i, std::size_t j) { return isvecg[i] < isvecg[j]; });
            // then sort by time
            std::stable_sort(perm.begin(), perm.end(),
                             [&](std::size_t i, std::size_t j) { return isvect[i] < isvect[j]; });
            // now apply permutation to time and gid output vectors
            std::transform(perm.begin(), perm.end(), osvect.begin(),
                           [&](std::size_t i) { return isvect[i]; });
            std::transform(perm.begin(), perm.end(), osvecg.begin(),
                           [&](std::size_t i) { return isvecg[i]; });
        }
    };



#else

    struct SerialImplementation {
        inline static int init() { std::cout << "INIT serial! " << std::endl; return 0; };
        inline static void init_comm(int reports) {};
        inline static void close(){};
        inline static std::tuple<hid_t, hid_t> prepare_write(hid_t plist_id) {};
        inline static hsize_t get_offset(hsize_t value) { return 0; };
        inline static hsize_t get_global_dims(hsize_t value) { return value; };
        inline static void sort_spikes(std::vector<double>& spikevec_time, std::vector<int>& spikevec_gid) {};
    };

#endif


}  // namespace detail

using Implementation = detail::Implementation<
#if defined(HAVE_MPI)
        detail::ParallelImplementation
#else
        detail::SerialImplementation
#endif
    >;