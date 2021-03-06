#include <utility>
#include "fast-slic.h"
#include "fast-slic-common-impl.hpp"


class Context : public BaseContext {};

static inline uint32_t get_assignment_value(const Cluster* cluster, const uint8_t* image, int32_t base_index, uint16_t spatial_dist, uint8_t quantize_level) {
    int32_t img_base_index = 3 * base_index;
    uint8_t r = image[img_base_index], g = image[img_base_index + 1], b = image[img_base_index + 2];
    uint16_t color_dist = ((uint32_t)(fast_abs<int16_t>(r - (int16_t)cluster->r) + fast_abs<int16_t>(g - (int16_t)cluster->g) + fast_abs<int16_t>(b - (int16_t)cluster->b)) << quantize_level);
    return ((uint32_t)(color_dist + spatial_dist) << 16) + (uint32_t)cluster->number;
}

static void slic_assign_cluster_oriented(Context *context) {
    auto H = context->H;
    auto W = context->W;
    auto K = context->K;
    auto clusters = context->clusters;
    auto image = context->image;
    auto assignment = context->assignment;
    auto quantize_level = context->quantize_level;
    auto spatial_normalize_cache = context->spatial_normalize_cache;

    const int16_t S = context->S;

    #if _OPENMP >= 200805
    #pragma omp parallel for collapse(2)
    #else
    #pragma omp parallel for
    #endif
    for (int i = 0; i < H; i++) {
        for (int j = 0; j < W; j++) {
            assignment[i * W + j] =  0xFFFFFFFF;
        }
    }

    // Sorting clusters by morton order seems to help for distributing clusters evenly for multiple cores
    std::vector<ZOrderTuple> cluster_sorted_tuples;
    {
        cluster_sorted_tuples.reserve(K);
        for (int k = 0; k < K; k++) {
            const Cluster* cluster = &clusters[k];
            uint32_t score = get_sort_value(cluster->y, cluster->x, S);
            cluster_sorted_tuples.push_back(ZOrderTuple(score, cluster));
        }
        std::sort(cluster_sorted_tuples.begin(), cluster_sorted_tuples.end());
    }

    // auto t1 = Clock::now();

    // OPTIMIZATION 1: floating point arithmatics is quantized down to int16_t
    // OPTIMIZATION 2: L1 norm instead of L2
    // OPTIMIZATION 5: assignment value is saved combined with distance and cluster number ([distance value (16 bit)] + [cluster number (16 bit)])
    // OPTIMIZATION 6: Make computations of L1 distance SIMD-friendly

    #pragma omp parallel for schedule(static)
    for (int cluster_sorted_idx = 0; cluster_sorted_idx < K; cluster_sorted_idx++) {
        const Cluster *cluster = cluster_sorted_tuples[cluster_sorted_idx].cluster;

        int16_t cluster_y = cluster->y;
        int16_t cluster_x = cluster->x;
        const int16_t y_lo = my_max<int16_t>(0, cluster_y - S), y_hi = my_min<int16_t>(H, cluster_y + S + 1);
        const int16_t x_lo = my_max<int16_t>(0, cluster_x - S), x_hi = my_min<int16_t>(W, cluster_x + S + 1);

        uint16_t row_first_manhattan = (cluster_y - y_lo) + (cluster_x - x_lo);
        for (int16_t i = y_lo; i < cluster_y; i++) {
            uint16_t current_manhattan = row_first_manhattan--;
            #pragma GCC unroll(2)
            for (int16_t j = x_lo; j < cluster_x; j++) {
                int32_t base_index = W * i + j;
                uint16_t spatial_dist = spatial_normalize_cache[current_manhattan--];
                uint32_t assignment_val = get_assignment_value(cluster, image, base_index, spatial_dist, quantize_level);
                if (assignment[base_index] > assignment_val)
                    assignment[base_index] = assignment_val;
            }

            #pragma GCC unroll(2)
            for (int16_t j = cluster_x; j < x_hi; j++) {
                int32_t base_index = W * i + j;
                uint16_t spatial_dist = spatial_normalize_cache[current_manhattan++];
                uint32_t assignment_val = get_assignment_value(cluster, image, base_index, spatial_dist, quantize_level);
                if (assignment[base_index] > assignment_val)
                    assignment[base_index] = assignment_val;
            }
        }

        for (int16_t i = cluster_y; i < y_hi; i++) {
            uint16_t current_manhattan = row_first_manhattan++;
            #pragma GCC unroll(2)
            for (int16_t j = x_lo; j < cluster_x; j++) {
                int32_t base_index = W * i + j;
                uint16_t spatial_dist = spatial_normalize_cache[current_manhattan--];
                uint32_t assignment_val = get_assignment_value(cluster, image, base_index, spatial_dist, quantize_level);
                if (assignment[base_index] > assignment_val)
                    assignment[base_index] = assignment_val;
            }

            #pragma GCC unroll(2)
            for (int16_t j = cluster_x; j < x_hi; j++) {
                int32_t base_index = W * i + j;
                uint16_t spatial_dist = spatial_normalize_cache[current_manhattan++];
                uint32_t assignment_val = get_assignment_value(cluster, image, base_index, spatial_dist, quantize_level);
                if (assignment[base_index] > assignment_val)
                    assignment[base_index] = assignment_val;
            }
        }

    }
    // auto t2 = Clock::now();

    // Clean up: Drop distance part in assignment and let only cluster numbers remain
    #if _OPENMP >= 200805
    #pragma omp parallel for collapse(2)
    #else
    #pragma omp parallel for
    #endif
    for (int i = 0; i < H; i++) {
        for (int j = 0; j < W; j++) {
            assignment[i * W + j] &= 0x0000FFFF; // drop the leading 2 bytes
        }
    }

    // std::cerr << "Tightloop: " << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count() << "us \n";

}

static void slic_assign(Context *context) {
    if (!strcmp(context->algorithm, "cluster_oriented")) {
        slic_assign_cluster_oriented(context);
    }
}


static void slic_update_clusters(Context *context) {
    auto H = context->H;
    auto W = context->W;
    auto K = context->K;
    auto image = context->image;
    auto clusters = context->clusters;
    auto assignment = context->assignment;

    int *num_cluster_members = new int[K];
    int *cluster_acc_vec = new int[K * 5]; // sum of [y, x, r, g, b] in cluster

    std::fill_n(num_cluster_members, K, 0);
    std::fill_n(cluster_acc_vec, K * 5, 0);

    #pragma omp parallel
    {
        int *local_acc_vec = new int [K * 5]; // sum of [y, x, r, g, b] in cluster
        int *local_num_cluster_members = new int[K];
        std::fill_n(local_num_cluster_members, K, 0);
        std::fill_n(local_acc_vec, K * 5, 0);
        #if _OPENMP >= 200805
        #pragma omp for collapse(2)
        #else
        #pragma omp for
        #endif
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < W; j++) {
                int base_index = W * i + j;
                int img_base_index = 3 * base_index;

                cluster_no_t cluster_no = (cluster_no_t)(assignment[base_index]);
                if (cluster_no == 0xFFFF) continue;
                local_num_cluster_members[cluster_no]++;
                local_acc_vec[5 * cluster_no + 0] += i;
                local_acc_vec[5 * cluster_no + 1] += j;
                local_acc_vec[5 * cluster_no + 2] += image[img_base_index];
                local_acc_vec[5 * cluster_no + 3] += image[img_base_index + 1];
                local_acc_vec[5 * cluster_no + 4] += image[img_base_index + 2];
            }
        }

        #pragma omp critical
        {
            for (int k = 0; k < K; k++) {
                for (int dim = 0; dim < 5; dim++) {
                    cluster_acc_vec[5 * k + dim] += local_acc_vec[5 * k + dim];
                }
                num_cluster_members[k] += local_num_cluster_members[k];
            }
        }
        delete [] local_acc_vec;
        delete [] local_num_cluster_members;
    }


    for (int k = 0; k < K; k++) {
        int num_current_members = num_cluster_members[k];
        Cluster *cluster = &clusters[k];
        cluster->num_members = num_current_members;

        if (num_current_members == 0) continue;

        // Technically speaking, as for L1 norm, you need median instead of mean for correct maximization.
        // But, I intentionally used mean here for the sake of performance.
        cluster->y = round_int(cluster_acc_vec[5 * k + 0], num_current_members);
        cluster->x = round_int(cluster_acc_vec[5 * k + 1], num_current_members);
        cluster->r = round_int(cluster_acc_vec[5 * k + 2], num_current_members);
        cluster->g = round_int(cluster_acc_vec[5 * k + 3], num_current_members);
        cluster->b = round_int(cluster_acc_vec[5 * k + 4], num_current_members);
    }
    delete [] num_cluster_members;
    delete [] cluster_acc_vec;
}

extern "C" {
    void fast_slic_initialize_clusters(int H, int W, int K, const uint8_t* image, Cluster *clusters) {
        do_fast_slic_initialize_clusters(H, W, K, image, clusters);
    }

    void fast_slic_iterate(int H, int W, int K, float compactness, float min_size_factor, uint8_t quantize_level, int max_iter, const uint8_t* image, Cluster* clusters, uint32_t* assignment) {

        Context context;
        context.image = image;
        context.algorithm = "cluster_oriented";
        context.H = H;
        context.W = W;
        context.K = K;
        context.S = (int16_t)sqrt(H * W / K);
        context.compactness = compactness;
        context.min_size_factor = min_size_factor;
        context.quantize_level = quantize_level;
        context.clusters = clusters;
        context.assignment = assignment;

        context.prepare_spatial();

        for (int i = 0; i < max_iter; i++) {
            // auto t1 = Clock::now();
            slic_assign(&context);
            // auto t2 = Clock::now();
            slic_update_clusters(&context);
            // auto t3 = Clock::now();
            // std::cerr << "assignment " << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count() << "us \n";
            // std::cerr << "update "<< std::chrono::duration_cast<std::chrono::microseconds>(t3-t2).count() << "us \n";
        }

        // auto t1 = Clock::now();
        slic_enforce_connectivity(&context);
        // auto t2 = Clock::now();

        // std::cerr << "enforce connectivity "<< std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count() << "us \n";
    }

    static uint32_t symmetric_int_hash(uint32_t x, uint32_t y) {
        /*
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = (x >> 16) ^ x;
        */
        return ((x * 0x1f1f1f1f) ^ y) +  ((y * 0x1f1f1f1f) ^ x);
    }

    Connectivity* fast_slic_get_connectivity(int H, int W, int K, const uint32_t *assignment) {
        const static int max_conn = 12;
        Connectivity* conn = new Connectivity();
        conn->num_nodes = K;
        conn->num_neighbors = new int[K];
        conn->neighbors = new uint32_t*[K];
        std::fill_n(conn->num_neighbors, K, 0);
        std::fill_n(conn->neighbors, K, nullptr);

        uint32_t *hashtable = new uint32_t[K];
        std::fill_n(hashtable, K, 0);

        for (int i = 0; i < K; i++) {
            conn->neighbors[i] = new uint32_t[max_conn];
        }

        for (int i = 0; i < H - 1; i++) {
            for (int j = 0; j < W - 1; j++) {
                int base_index = W * i + j;
                const uint32_t source = assignment[base_index];
                int num_source_neighbors = conn->num_neighbors[source];
                if (source >= (uint32_t)K) continue;
                uint32_t* source_neighbors = conn->neighbors[source];

#               define CONNECTIVITY_SEARCH(expr) do { \
                    int target_index = expr; \
                    const uint32_t target = assignment[target_index];\
                    if (target >= (uint32_t)K || source == target) continue; \
                    const int num_target_neighbors = conn->num_neighbors[target]; \
                    if (num_source_neighbors >= max_conn || num_target_neighbors >= max_conn) continue; \
                    uint32_t* target_neighbors = conn->neighbors[target]; \
                    int hash_idx = symmetric_int_hash(source, target) % (K * 32); \
                    if (hashtable[hash_idx / 32] & (1 << (hash_idx % 32))) { \
                        bool exists = false; \
                        for (int t = 0; t < num_source_neighbors; t++) { \
                            if (source_neighbors[t] == target) { \
                                exists = true; \
                                break; \
                            } \
                        } \
                        if (exists) continue; \
                        for (int t = 0; t < num_target_neighbors; t++) { \
                            if (target_neighbors[t] == source) { \
                                exists = true; \
                                break; \
                            } \
                        } \
                        if (exists) continue; \
                    } \
                    target_neighbors[conn->num_neighbors[target]++] = source; \
                    source_neighbors[num_source_neighbors++] = target; \
                    hashtable[hash_idx / 32] |= (1 << (hash_idx % 32)); \
                } while (0);
                CONNECTIVITY_SEARCH(base_index + 1);
                CONNECTIVITY_SEARCH(base_index + W);
                CONNECTIVITY_SEARCH(base_index + W + 1);
                conn->num_neighbors[source] = num_source_neighbors;
            }
        }

        delete [] hashtable;
        return conn;
    }

    Connectivity* fast_slic_knn_connectivity(int H, int W, int K, const Cluster* clusters, size_t num_neighbors) {
        // auto t1 = Clock::now();
        int S = my_max((int)sqrt(H * W / K), 1);
        int nh = ceil_int(H, S), nw = ceil_int(W, S);

        std::vector< std::vector<const Cluster*> > s_cells(nh * nw);
        for (int i = 0; i < K; i++) {
            const Cluster* cluster = clusters + i;
            s_cells[(cluster->y / S) * nw + (cluster->x / S)].push_back(cluster);
        }

        // auto t2 = Clock::now();
        Connectivity* conn = new Connectivity();
        conn->num_nodes = K;
        conn->num_neighbors = new int[K];
        conn->neighbors = new uint32_t*[K];

        #pragma omp parallel for
        for (int i = 0; i < K; i++) {
            const Cluster* cluster = clusters + i;
            int cell_center_x = cluster->x / S, cell_center_y = cluster->y / S;

            std::vector<std::pair<int, const Cluster*>> heap;
            for (int cy = my_max(cell_center_y - 3, 0); cy < my_min(nh, cell_center_y + 3); cy++) {
                for (int cx = my_max(cell_center_x - 3, 0); cx < my_min(nw, cell_center_x + 3); cx++) {
                    for (const Cluster* cluster_around  : s_cells[cy * nw + cx])  {
                        if (cluster_around == cluster) continue;
                        int distance = fast_abs(cluster_around->x - cluster->x) + fast_abs(cluster_around->y - cluster->y);
                        if (!heap.empty() && heap.front().first <= distance) continue;

                        heap.push_back(std::pair<int, const Cluster*>(distance, cluster_around));
                        std::push_heap(heap.begin(), heap.end());
                        while (heap.size() > num_neighbors) {
                            std::pop_heap(heap.begin(), heap.end());
                            heap.pop_back();
                        }
                    }
                }
            }
            conn->num_neighbors[i] = heap.size();
            conn->neighbors[i] = new uint32_t[heap.size()];
            for (size_t j = 0; j < heap.size(); j++) {
                conn->neighbors[i][j] = heap[j].second->number;
            }
        }
        // auto t3 = Clock::now();

        // std::cerr << "Build " << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count() << "us \n";
        // std::cerr << "Find " << std::chrono::duration_cast<std::chrono::microseconds>(t3-t2).count() << "us \n";
        return conn;
    }

    void fast_slic_free_connectivity(Connectivity* conn) {
        delete [] conn->num_neighbors;
        for (int i = 0; i < conn->num_nodes; i++) {
            delete [] conn->neighbors[i];
        }
        delete [] conn->neighbors;
        delete conn;
    }

    void fast_slic_get_mask_density(int H, int W, int K, const Cluster *clusters, const uint32_t* assignment, const uint8_t *mask, uint8_t *cluster_densities) {
        int *sum = new int[K];
        std::fill_n(sum, K, 0);
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < W; j++) {
                uint32_t cluster_no = assignment[W * i + j];
                if (cluster_no < (uint32_t)K)
                    sum[cluster_no] += mask[W * i + j];
            }
        }

        for (int k = 0; k < K; k++) {
            cluster_densities[k] = (uint8_t)my_min<int>(255, sum[k] / my_max(clusters[k].num_members, 1u));
        }
        delete [] sum;
    }

    void fast_slic_cluster_density_to_mask(int H, int W, int K, const Cluster *clusters, const uint32_t* assignment, const uint8_t *cluster_densities, uint8_t *result) {
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < W; j++) {
                uint32_t cluster_no = assignment[W * i + j];
                if (cluster_no < (uint32_t)K)
                    result[W * i + j] = cluster_densities[cluster_no];
                else
                    result[W * i + j] = 0;
            }
        }
    }
}

#ifdef PROTOTYPE_MAIN_DEMO
#include <string>
#include <chrono>
#include <fstream>
#include <memory>
#include <iostream>
typedef std::chrono::high_resolution_clock Clock;
int main(int argc, char** argv) {
    int K = 100;
    int compactness = 5;
    int max_iter = 2;
    int quantize_level = 7;
    try { 
        if (argc > 1) {
            K = std::stoi(std::string(argv[1]));
        }
        if (argc > 2) {
            compactness = std::stoi(std::string(argv[2]));
        }
        if (argc > 3) {
            max_iter = std::stoi(std::string(argv[3]));
        }
        if (argc > 4) {
            quantize_level = std::stoi(std::string(argv[4]));
        }
    } catch (...) {
        std::cerr << "slic num_components compactness max_iter quantize_level" << std::endl;
        return 2;
    }

    int H = 480;
    int W = 640;
    Cluster clusters[K];
    std::unique_ptr<uint8_t[]> image { new uint8_t[H * W * 3] };
    std::unique_ptr<uint32_t[]> assignment { new uint32_t[H * W] };

    std::ifstream inputf("/tmp/a.txt");
    for (int i = 0; i < H; i++) {
        for (int j = 0; j < W; j++) {
            int r, g, b;
            inputf >> r >> g >> b;
            image.get()[3 * W * i + 3 * j] = r;
            image.get()[3 * W * i + 3 * j + 1] = g;
            image.get()[3 * W * i + 3 * j + 2] = b;
        }
    }

    auto t1 = Clock::now();
    fast_slic_initialize_clusters(H, W, K, image.get(), clusters);
    fast_slic_iterate(H, W, K, compactness, 0.1, quantize_level, max_iter, image.get(), clusters, assignment.get());

    auto t2 = Clock::now();
    // 6 times faster than skimage.segmentation.slic
    std::cerr << std::chrono::duration_cast<std::chrono::microseconds>(t2-t1).count() << "us \n";

    {
        std::ofstream outputf("/tmp/b.output.txt");
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < W; j++) {
                outputf << (short)assignment.get()[W * i + j] << " ";
            }
            outputf << std::endl;
        }
    }
    {
        std::ofstream outputf("/tmp/b.clusters.txt");
        for (int k = 0; k < K; k++) {
            outputf << clusters[k].y << " " << clusters[k].x << " " << clusters[k].num_members << std::endl;
        }
    }
    return 0;
}
#endif
