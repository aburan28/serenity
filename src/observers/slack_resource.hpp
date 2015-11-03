#ifndef SERENITY_SLACK_RESOURCE_HPP
#define SERENITY_SLACK_RESOURCE_HPP

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "mesos/mesos.hpp"
#include "mesos/resources.hpp"

#include "stout/result.hpp"

#include "serenity/executor_set.hpp"
#include "serenity/serenity.hpp"

namespace mesos {
namespace serenity {

// TODO(bplotka): Make default role configurable from another source
// (not only env)and as a default pass *
inline std::string getDefaultRole() {
  if (const char* env = std::getenv("MESOS_DEFAULT_ROLE")) {
    return env;
  }
  return "*";
}


/**
 * SlackResourceObserver observes incoming ResourceUsage
 * and produces Resource with revocable flag set (Slack Resources).
 *
 * Currently it only counts CPU slack
 */
class SlackResourceObserver : public Consumer<ResourceUsage>,
                              public Producer<Resources> {
 public:
  SlackResourceObserver(
      double_t _maxOversubscriptionFraction =
      DEFAULT_MAX_OVERSUBSCRIPTION_FRACTION)
      : previousSamples(new ExecutorSet()),
        maxOversubscriptionFraction(_maxOversubscriptionFraction),
        default_role(getDefaultRole()) {}

  explicit SlackResourceObserver(
      Consumer<Resources>* _consumer,
      double_t _maxOversubscriptionFraction =
        DEFAULT_MAX_OVERSUBSCRIPTION_FRACTION) :
      Producer<Resources>(_consumer),
      maxOversubscriptionFraction(_maxOversubscriptionFraction),
      previousSamples(new ExecutorSet()),
      default_role(getDefaultRole()) {}

  ~SlackResourceObserver() {}

  Try<Nothing> consume(const ResourceUsage& usage) override;

 protected:
  Result<double_t> CalculateCpuSlack(const ResourceUsage_Executor& prev,
                                     const ResourceUsage_Executor& current)
                                     const;

  std::unique_ptr<ExecutorSet> previousSamples;

  /**
   * Report up to maxOversubscriptionFraction of
   * total Agent's CPU resources as slack resources
   */
  double_t maxOversubscriptionFraction;

  static constexpr const double_t DEFAULT_MAX_OVERSUBSCRIPTION_FRACTION = 0.8;

  /** Don't report slack when it's less than this value */
  static constexpr const double_t SLACK_EPSILON = 0.001;

  /** Name of the class for logging purposes */
  static constexpr const char* NAME = "[Serenity] SlackObserver: ";

 private:
  SlackResourceObserver(const SlackResourceObserver& other)
      : default_role(getDefaultRole()) {}
  std::string default_role;
};

}  // namespace serenity
}  // namespace mesos

#endif  // SERENITY_SLACK_RESOURCE_HPP