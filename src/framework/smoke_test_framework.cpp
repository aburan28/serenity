/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define PICOJSON_USE_INT64

#include <list>
#include <string>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>
#include <mesos/type_utils.hpp>

#include <stout/exit.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "common/protobuf_utils.hpp"
#include "common/status_utils.hpp"

#include "logging/logging.hpp"

#include "smoke_flags.hpp"
#include "smoke_job.hpp"

using namespace mesos;
using namespace mesos::internal;

using std::list;
using std::string;
using std::vector;

using mesos::Resources;


static Offer::Operation LAUNCH(const vector<TaskInfo>& tasks)
{
  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH);

  foreach (const TaskInfo& task, tasks) {
    operation.mutable_launch()->add_task_infos()->CopyFrom(task);
  }

  return operation;
}


/**
 * Serenity No Executor Scheduler based on Mesos No Executor Scheduler.
 */
class SerenityNoExecutorScheduler : public Scheduler
{
public:
  SerenityNoExecutorScheduler(
      const FrameworkInfo& _frameworkInfo,
      const list<SmokeJob>& _jobs)
    : frameworkInfo(_frameworkInfo),
      tasksLaunched(0u),
      tasksFinished(0u),
      tasksTerminated(0u),
      jobsScheduled(0u) {
    foreach (const SmokeJob& job, _jobs) {
        if(job.isEndless()) {
          endlessJobs.push_back(SmokeJob(job));
        } else {
          // TODO(bplotka): Sort base on priority.
          limitedJobs.push_back(SmokeJob(job));
        }
    }

    if (endlessJobs.size() > 1) {
      // TODO(bplotka): Add support for > 1 unlimited jobs.
      LOG(WARNING) << "Currently there is support only for one unlimited"
        << "job. Only first task (cmd:" << endlessJobs[0].command
        << " will be scheduled.";
    }

    LOG(INFO) << "SerenityNoExecutorScheduler initialized.";
  }

  virtual void registered(
      SchedulerDriver* driver,
      const FrameworkID& frameworkId,
      const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Registered with master " << masterInfo
              << " and got framework ID " << frameworkId;

    frameworkInfo.mutable_id()->CopyFrom(frameworkId);
  }

  virtual void reregistered(
      SchedulerDriver* driver,
      const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Reregistered with master " << masterInfo;
  }

  virtual void disconnected(
      SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected!";
  }

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers)
  {
    Filters filters;
    filters.set_refuse_seconds(Duration::max().secs());
    foreach (const Offer& offer, offers) {
      // Check each offer.
      if (!this->endlessMode() && this->allLimitedJobsScheduled()) {
        // In case of end of our scheduling - fully resign from any offer.
        driver->declineOffer(offer.id(), filters);
        continue;
      }
      LOG(INFO) << "Received offer " << offer.id() << " from slave "
                << offer.slave_id() << " (" << offer.hostname() << ") "
                << "with " << offer.resources();

      Resources remaining = offer.resources();
      vector<TaskInfo> tasks;

      SmokeJob* job = nullptr;
      if (!allLimitedJobsScheduled()) {
        // Get job from limited job vector.
        job = &limitedJobs[jobsScheduled];
      } else {
        // Get job from unlimited job vector (only first).
        job = &endlessJobs[0];
      }

      if (job->targetHostname.isSome() &&
          job->targetHostname.get().compare(offer.hostname()) != 0){
        // Host don't match.
        LOG(INFO) << "Offered host " << offer.hostname()
        << " not matched with target " << job->targetHostname.get()
        << ". Omitting.";
        continue;
      }

      while (true) {
        // Check if there are still resources for next task.
        if (!remaining.contains(job->taskResources)) {
          LOG(INFO) << "Not enough resources for "
          << stringify(jobsScheduled) + "_"
             + stringify(job->tasksLaunched)
          << " job. Needed: " << job->taskResources
          << " Offered: " << remaining;
          break;
        }

        remaining -= job->taskResources;

        tasks.push_back(
          job->createTask(jobsScheduled, offer.slave_id()));

        this->activeTasks.insert(tasks.back().task_id());
        job->tasksLaunched++;
        tasksLaunched++;
        LOG(INFO) << "Launching " << tasks.back().task_id();

        if (!job->isEndless() &&
            job->tasksLaunched  >= job->totalTasks.get()) {
          // In case of limited jobs stop when scheduled totalTasks.
          job->scheduled = true;

          this->jobsScheduled++;
          break;
        }
      }

      driver->acceptOffers({offer.id()}, {LAUNCH(tasks)});
    }
  }

  virtual void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId)
  {
    LOG(INFO) << "Offer " << offerId << " has been rescinded";
  }

  virtual void statusUpdate(
      SchedulerDriver* driver,
      const TaskStatus& status)
  {
    if (!activeTasks.contains(status.task_id())) {
      LOG(WARNING) << "Unknown task '" << status.task_id() << "'"
                   << " is in state " << status.state();
      return;
    }

    if (status.state() == TASK_LOST ||
        status.state() == TASK_KILLED ||
        status.state() == TASK_FAILED) {
      LOG(ERROR) << "Task '" << status.task_id() << "'"
                 << " is in unexpected state " << status.state()
                 << (status.has_reason()
                     ? " with reason " + stringify(status.reason()) : "")
                 << " from source " << status.source()
                 << " with message '" << status.message() << "'";
    } else {
      LOG(INFO) << "Task '" << status.task_id() << "'"
                << " is in state " << status.state();
    }

    if (internal::protobuf::isTerminalState(status.state())) {
      if (status.state() == TASK_FINISHED) {
        tasksFinished++;
      }

      tasksTerminated++;
      activeTasks.erase(status.task_id());
    }

    // In Endless Mode we can stop framework only by killing or sigterming it.
    if (!endlessMode() && allLimitedJobsScheduled() &&
        tasksTerminated >= tasksLaunched) {
      if (tasksTerminated - tasksFinished > 0) {
        EXIT(EXIT_FAILURE)
          << "Failed to complete successfully: "
          << stringify(tasksTerminated - tasksFinished)
          << " of " << stringify(tasksLaunched) << " terminated abnormally";
      } else {
        LOG(INFO) << "Stopping framework.";
        driver->stop();
      }
    }
  }

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data)
  {
    LOG(FATAL) << "Received framework message from executor '" << executorId
               << "' on slave " << slaveId << ": '" << data << "'";
  }

  virtual void slaveLost(
      SchedulerDriver* driver,
      const SlaveID& slaveId)
  {
    LOG(INFO) << "Lost slave " << slaveId;
  }

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status)
  {
    LOG(INFO) << "Lost executor '" << executorId << "' on slave "
              << slaveId << ", " << WSTRINGIFY(status);
  }

  virtual void error(
      SchedulerDriver* driver,
      const string& message)
  {
    LOG(ERROR) << message;
  }

private:
  FrameworkInfo frameworkInfo;
  vector<SmokeJob> limitedJobs;
  vector<SmokeJob> endlessJobs;
  size_t tasksLaunched;
  size_t tasksFinished;
  size_t tasksTerminated;
  hashset<TaskID> activeTasks;
  size_t jobsScheduled;

  bool endlessMode() {
    return (endlessJobs.size() > 0);
  }

  bool allLimitedJobsScheduled() {
    return jobsScheduled >= limitedJobs.size();
  }

};


int main(int argc, char** argv)
{
  bool enableRevocable = false;
  SmokeFlags flags;

  Try<Nothing> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE)
      << flags.usage(load.error());
  }

  if (flags.help) {
    EXIT(EXIT_SUCCESS)
      << flags.usage();
  }

  if (flags.master.isNone()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Missing required option --master");
  }

  if (flags.principal.isSome() != flags.secret.isSome()) {
    EXIT(EXIT_FAILURE)
      << flags.usage("Both --principal and --secret are required"
                     " to enable authentication");
  }

  logging::initialize(argv[0], flags, true); // Catch signals.

  FrameworkInfo framework;
  framework.set_user(""); // Have Mesos fill in the current user.
  framework.set_name("Serenity Smoke Test Framework");
  framework.set_checkpoint(flags.checkpoint);
  framework.set_role(flags.role);

  list<SmokeJob> jobs;
  if (flags.tasks_json_path.isSome())
  {
    jobs = SmokeJob::createJobsFromJson(flags, enableRevocable);

  } else {
    // Task specification.
    Try<Resources> resources =
        Resources::parse(flags.task_resources);

    if (resources.isError()) {
      EXIT(EXIT_FAILURE)
      << flags.usage("Invalid --task_resources: " +
                     resources.error());
    }

    Resources taskResources = resources.get();

    if (flags.task_revocable_resources.isSome()) {
      enableRevocable = true;
      Try<Resources> revocableResources =
          Resources::parse(flags.task_revocable_resources.get());

      if (revocableResources.isError()) {
        EXIT(EXIT_FAILURE)
        << flags.usage("Invalid --task_revocable_resources: " +
                       revocableResources.error());
      }

      foreach (Resource revocable, revocableResources.get()) {
          revocable.mutable_revocable();
          taskResources += revocable;
      }
    }

    Option<SmokeURI> uri = None();
    if (flags.uri_value.isSome()) {
      uri = SmokeURI(flags.uri_value.get());
    }
    jobs.push_back(
      SmokeJob(flags.command,
               taskResources,
               flags.num_tasks,
               flags.target_hostname,
               uri));
  }

  if (enableRevocable) {
    LOG(INFO) << "Enabled getting revocable resources.";
    framework.add_capabilities()->set_type(
        FrameworkInfo::Capability::REVOCABLE_RESOURCES);
  }

  if (flags.principal.isSome()) {
    framework.set_principal(flags.principal.get());
  }

  SerenityNoExecutorScheduler scheduler(framework, jobs);

  MesosSchedulerDriver* driver;

  if (flags.principal.isSome() && flags.secret.isSome()) {
    Credential credential;
    credential.set_principal(flags.principal.get());
    credential.set_secret(flags.secret.get());

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get(), credential);
  } else {
    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master.get());
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}