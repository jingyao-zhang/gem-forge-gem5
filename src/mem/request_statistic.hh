/**
 * @author: Zhengrong Wang
 * @file
 * There is a need to collect statistics of a specific request.
 * This class should serve as a placeholder of the statistics of
 * a request. The cache hierarchy can set it if it is presented.
 *
 * Ruby system can also support this statistics.
 */

#ifndef __MEM_REQUEST_STATISTIC_HH__
#define __MEM_REQUEST_STATISTIC_HH__

struct RequestStatistic {
  int hitCacheLevel;
  RequestStatistic() : hitCacheLevel(0) {}
  void setHitCacheLevel(int hitCacheLevel) {
    this->hitCacheLevel = hitCacheLevel;
  }
};
#endif