#include "anomalydetector.h"
#include <random>
#include <stdexcept>

#include <boost/exception/all.hpp>

namespace Akumuli {
namespace QP {


static uint32_t combine(uint32_t hi, uint32_t lo) {
    return (uint32_t)(2 - (int)hi + (int)lo);
}


//! Family of 4-universal hash functions
struct HashFnFamily {
    const uint32_t N;
    const uint32_t K;
    //! Tabulation based hash fn used, N tables should be generated using RNG in c-tor
    std::vector<std::vector<unsigned short>> table_;

    //! C-tor. N - number of different hash functions, K - number of values (should be a power of two)
    HashFnFamily(uint32_t N, uint32_t K)
        : N(N)
        , K(K)
    {
        // N should be odd
        if (N % 2 == 0) {
            std::runtime_error err("invalid argument N (should be odd)");
            BOOST_THROW_EXCEPTION(err);
        }
        // K should be a power of two
        auto mask = K-1;
        if ((mask&K) != 0) {
            std::runtime_error err("invalid argument K (should be a power of two)");
            BOOST_THROW_EXCEPTION(err);
        }
        // Generate tables
        std::random_device randdev;
        std::mt19937 generator(randdev());
        std::uniform_int_distribution<> distribution;
        for (uint32_t i = 0; i < N; i++) {
            std::vector<unsigned short> col;
            auto mask = K-1;
            for (int j = 0; j < 0x10000; j++) {
                int value = distribution(generator);
                col.push_back((uint32_t)mask&value);
            }
            table_.push_back(col);
        }
    }

    //! Calculate hash value in range [0, K)
    uint32_t hash(int ix, uint64_t key) const {
        auto hi32 = key >> 32;
        auto lo32 = key & 0xFFFFFFFF;
        auto hilo = combine(hi32, lo32);

        auto hi32hash = hash32(ix, hi32);
        auto lo32hash = hash32(ix, lo32);
        auto hilohash = hash32(ix, hilo);

        return hi32hash ^ lo32hash ^ hilohash;
    }

private:
    uint32_t hash32(int ix, uint32_t key) const {
        auto hi16 = key >> 16;
        auto lo16 = key & 0xFFFF;
        auto hilo = combine(hi16, lo16);
        return table_[ix][lo16] ^ table_[ix][hi16] ^ table_[ix][hilo];
    }
};



//                          //
//      CountingSketch      //
//                          //


struct CountingSketch {
    HashFnFamily const& hashes_;
    const uint32_t N;
    const uint32_t K;
    double sum_;
    std::vector<std::vector<double>> tables_;

    CountingSketch(HashFnFamily const& hf)
        : hashes_(hf)
        , N(hf.N)
        , K(hf.K)
        , sum_(0.0)
    {
        for (uint32_t i = 0u; i < N; i++) {
            std::vector<double> row;
            row.resize(K, 0.0);
            tables_.push_back(std::move(row));
        }
    }

    CountingSketch(CountingSketch const& cs)
        : hashes_(cs.hashes_)
        , N(cs.N)
        , K(cs.K)
        , sum_(cs.sum_)
    {
        for (auto ixrow = 0u; ixrow < N; ixrow++) {
            std::vector<double> row;
            row.resize(K, 0.0);
            std::vector<double> const& rcs = cs.tables_[ixrow];
            for (auto col = 0u; col < K; col++) {
                row[col] = rcs[col];
            }
            tables_.push_back(std::move(row));
        }
    }

    void _update_sum() {
        sum_ = 0.0;
        for (auto val: tables_[0]) {
            sum_ += val;
        }
    }

    void add(uint64_t id, double value) {
        sum_ += value;
        for (uint32_t i = 0; i < N; i++) {
            // calculate hash from id to K
            uint32_t hash = hashes_.hash(i, id);
            tables_[i][hash] += value;
        }
    }

    //! Second moment estimator
    double estimateF2() const {
        std::vector<double> results;
        auto f = 1./(K - 1);
        for (uint32_t i = 0u; i < N; i++) {
            double rowsum = std::accumulate(tables_[i].begin(), tables_[i].end(), 0.0, [](double acc, double val) {
                return acc + val*val;
            });
            double res = K*f*sqrt(rowsum) - f*sum_*sum_;
            results.push_back(res);
        }
        std::sort(results.begin(), results.end());
        return results[N/2];
    }

    //! Unbiased value estimator
    double estimate(uint64_t id) const {
        std::vector<double> results;
        for (uint32_t i = 0u; i < N; i++) {
            uint32_t hash = hashes_.hash(i, id);
            double value = tables_[i][hash];
            double estimate = (value - sum_/K)/(1. - 1./K);
            results.push_back(estimate);
        }
        std::sort(results.begin(), results.end());
        return results[N/2];
    }

    //! current sketch <- absolute difference between two arguments
    void diff(CountingSketch const& lhs, CountingSketch const& rhs) {
        for (auto ixrow = 0u; ixrow < N; ixrow++) {
            std::vector<double>& row = tables_[ixrow];
            std::vector<double> const& lrow = lhs.tables_[ixrow];
            std::vector<double> const& rrow = rhs.tables_[ixrow];
            for (auto col = 0u; col < K; col++) {
                row[col] = abs(lrow[col] - rrow[col]);
            }
        }
        _update_sum();
    }

    //! Add sketch
    void add(CountingSketch const& val) {
        for (auto ixrow = 0u; ixrow < N; ixrow++) {
            std::vector<double>& row = tables_[ixrow];
            std::vector<double> const& rval = val.tables_[ixrow];
            for (auto col = 0u; col < K; col++) {
                row[col] = row[col] + rval[col];
            }
        }
        _update_sum();
    }

    //! Substract sketch
    void sub(CountingSketch const& val) {
        for (auto ixrow = 0u; ixrow < N; ixrow++) {
            std::vector<double>& row = tables_[ixrow];
            std::vector<double> const& rval = val.tables_[ixrow];
            for (auto col = 0u; col < K; col++) {
                row[col] = row[col] - rval[col];
            }
        }
        _update_sum();
    }

    //! Multiply sketch by value
    void mul(double value) {
        for (auto ixrow = 0u; ixrow < N; ixrow++) {
            std::vector<double>& row = tables_[ixrow];
            for (auto col = 0u; col < K; col++) {
                row[col] *= value;
            }
        }
        _update_sum();
    }
};


//                          //
//      PreciseCounter      //
//                          //

struct PreciseCounter {
    std::unordered_map<uint64_t, double> table_;

    //! C-tor. Parameter `hf` is unused for the sake of interface unification.
    PreciseCounter(HashFnFamily const& hf) {
    }

    PreciseCounter(PreciseCounter const& cs)
        : table_(cs.table_)
    {
    }

    void add(uint64_t id, double value) {
        table_[id] += value;
    }

    //! Unbiased value estimator
    double estimate(uint64_t id) const {
        auto it = table_.find(id);
        if (it != table_.end()) {
            return it->second;
        }
        return 0.;
    }

    //! Second moment estimator
    double estimateF2() const {
        double sum = std::accumulate(table_.begin(), table_.end(), 0.0,
                                     [](double acc, std::pair<uint64_t, double> pval) {
            return acc + pval.second*pval.second;
        });
        return sqrt(sum);
    }

    //! current sketch <- absolute difference between two arguments
    void diff(PreciseCounter const& lhs, PreciseCounter const& rhs) {
        const std::unordered_map<uint64_t, double> *small, *large;
        if (lhs.table_.size() < rhs.table_.size()) {
            small = &lhs.table_;
            large = &rhs.table_;
        } else {
            small = &rhs.table_;
            large = &lhs.table_;
        }
        table_.clear();
        // Scan largest
        for (auto it = large->begin(); it != large->end(); it++) {
            auto small_it = small->find(it->first);
            double val = 0.;
            if (small_it != small->end()) {
                val = small_it->second;
            }
            table_[it->first] = abs(it->second - val);
        }
    }

    //! Add sketch
    void add(PreciseCounter const& val) {
        for(auto it = val.table_.begin(); it != val.table_.end(); it++) {
            table_[it->first] += it->second;
        }
    }

    //! Substract sketch
    void sub(PreciseCounter const& val) {
        for(auto it = val.table_.begin(); it != val.table_.end(); it++) {
            table_[it->first] -= it->second;
        }
    }

    //! Multiply sketch by value
    void mul(double value) {
        for(auto it = table_.begin(); it != table_.end(); it++) {
            it->second *= value;
        }
    }
};


//                              //
//      SMASlidingWindow        //
//                              //


//! Simple moving average implementation
template<class Frame>
struct SMASlidingWindow {
    typedef std::unique_ptr<Frame> PFrame;
    PFrame             sma_;
    const uint32_t     depth_;
    const double       mul_;
    std::deque<PFrame> queue_;

    SMASlidingWindow(uint32_t depth)
        : depth_(depth)
        , mul_(1.0/depth)
    {
    }

    void add(PFrame sketch) {
        if (!sma_) {
            sma_.reset(new Frame(*sketch));
            queue_.push_back(std::move(sketch));
        } else {
            sma_->add(*sketch);
            queue_.push_back(std::move(sketch));
            if (queue_.size() > depth_) {
                auto removed = std::move(queue_.front());
                queue_.pop_front();
                sma_->sub(*removed);
            }
        }
    }

    PFrame forecast() const {
        PFrame res;
        if (queue_.size() < depth_) {
            // return empty response
            return std::move(res);
        }
        res.reset(new Frame(*sma_));
        res->mul(mul_);
        return std::move(res);
    }
};


//                              //
//      EWMASlidingWindow       //
//                              //


//! Exponentialy weighted moving average implementation
template<class Frame>
struct EWMASlidingWindow {
    typedef std::unique_ptr<Frame> PFrame;
    PFrame               ewma_;
    const double         decay_;
    int                  counter_;

    EWMASlidingWindow(uint32_t depth)
        : decay_(1.0/(double(depth) + 1.0))
        , counter_(0.0)
    {
    }

    void add(PFrame sketch) {
        if (!ewma_) {
            ewma_.reset(new Frame(*sketch));
            counter_ = 1;
        } else if (counter_ < 10) {
            ewma_->add(*sketch);
            counter_++;
            if (counter_ == 10) {
                ewma_->mul(0.1);
            }
        } else {
            sketch->mul(decay_);
            ewma_->mul(1.0 - decay_);
            ewma_->add(*sketch);
        }
    }

    PFrame forecast() const {
        PFrame res;
        if (counter_ < 10) {
            // return empty response
            return std::move(res);
        }
        res.reset(new Frame(*ewma_));
        return std::move(res);
    }
};


//                                  //
//      AnomalyDetectorPipeline     //
//                                  //

template<
    class Frame,                        // Frame type
    template<class F> class FMethod     // Forecasting method type
>
struct AnomalyDetectorPipeline : AnomalyDetectorIface {
    typedef FMethod<Frame>                  FcastMethod;
    typedef std::unique_ptr<Frame>          PFrame ;
    typedef std::unique_ptr<FcastMethod>    PSlidingWindow;

    HashFnFamily                hashes_;
    const uint32_t              N;
    const uint32_t              K;
    PFrame                      current_;
    PFrame                      error_;
    double                      F2_;
    double                      threshold_;
    PSlidingWindow              sliding_window_;

    AnomalyDetectorPipeline(uint32_t N, uint32_t K, double threshold, PSlidingWindow swindow)
        : hashes_(N, K)
        , N(N)
        , K(K)
        , F2_(0.0)
        , threshold_(threshold)
        , sliding_window_(std::move(swindow))
    {
        current_.reset(new Frame(hashes_));
    }

    void add(uint64_t id, double value) {
        current_->add(id, value);
    }

    //! Returns true if series is anomalous (approx)
    bool is_anomaly_candidate(uint64_t id) const {
        if (error_) {
            double estimate = error_->estimate(id);
            return estimate > F2_;
        }
        return false;
    }

    void move_sliding_window() {
        PFrame forecast = std::move(sliding_window_->forecast());
        if (forecast) {
            error_ = std::move(calculate_error(forecast, current_));
            F2_ = sqrt(error_->estimateF2())*threshold_;
        }
        sliding_window_->add(std::move(current_));
        current_.reset(new Frame(hashes_));
    }

    PFrame calculate_error(const PFrame &forecast, const PFrame &actual) {
        PFrame res;
        res.reset(new Frame(hashes_));
        res->diff(*forecast, *actual);
        return std::move(res);
    }
};

template<class Window, class Detector>
std::unique_ptr<AnomalyDetectorIface> create_detector(uint32_t N,
                                                      uint32_t K,
                                                      double threshold,
                                                      uint32_t window_size)
{
    std::unique_ptr<AnomalyDetectorIface> result;
    std::unique_ptr<Window> window(new Window(window_size));
    result.reset(new Detector(N, K, threshold, std::move(window)));
    return std::move(result);
}

std::unique_ptr<AnomalyDetectorIface> AnomalyDetectorUtil::create_sma(uint32_t N,
                                                                      uint32_t K,
                                                                      double threshold,
                                                                      uint32_t window_size,
                                                                      bool approx)
{
    typedef AnomalyDetectorPipeline<PreciseCounter, SMASlidingWindow>   PreciseSMADetector;
    typedef SMASlidingWindow<PreciseCounter>                            PreciseSMAWindow;
    typedef AnomalyDetectorPipeline<CountingSketch, SMASlidingWindow>   SketchSMADetector;
    typedef SMASlidingWindow<CountingSketch>                            SketchSMAWindow;

    std::unique_ptr<AnomalyDetectorIface> result;

    if (approx) {
        result = create_detector<SketchSMAWindow, SketchSMADetector>(N, K, threshold, window_size);
        return std::move(result);
    } else {
        result = create_detector<PreciseSMAWindow, PreciseSMADetector>(N, K, threshold, window_size);
        return std::move(result);
    }
}

std::unique_ptr<AnomalyDetectorIface> AnomalyDetectorUtil::create_ewma(uint32_t N,
                                                                      uint32_t K,
                                                                      double threshold,
                                                                      uint32_t window_size,
                                                                      bool approx)
{
    typedef AnomalyDetectorPipeline<PreciseCounter, EWMASlidingWindow>  PreciseEWMADetector;
    typedef EWMASlidingWindow<PreciseCounter>                           PreciseEWMAWindow;
    typedef AnomalyDetectorPipeline<CountingSketch, EWMASlidingWindow>  SketchEWMADetector;
    typedef EWMASlidingWindow<CountingSketch>                           SketchEWMAWindow;

    std::unique_ptr<AnomalyDetectorIface> result;

    if (approx) {
        result = create_detector<SketchEWMAWindow, SketchEWMADetector>(N, K, threshold, window_size);
        return std::move(result);
    } else {
        result = create_detector<PreciseEWMAWindow, PreciseEWMADetector>(N, K, threshold, window_size);
        return std::move(result);
    }
}

}
}
