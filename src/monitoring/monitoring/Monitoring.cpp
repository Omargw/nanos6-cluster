
#include <fstream>

#include <config.h>

#include "Monitoring.hpp"


EnvironmentVariable<bool> Monitoring::_enabled("NANOS6_MONITORING_ENABLE", true);
EnvironmentVariable<bool> Monitoring::_verbose("NANOS6_MONITORING_VERBOSE", true);
EnvironmentVariable<std::string> Monitoring::_outputFile("NANOS6_MONITORING_VERBOSE_FILE", "output-monitoring.txt");
Monitoring *Monitoring::_monitor;


//    MONITORING    //

void Monitoring::initialize()
{
	if (_enabled) {
		#if CHRONO_ARCH
			// Start measuring time to compute the tick conversion rate
			TickConversionUpdater::initialize();
		#endif
		
		// Create the monitoring module
		if (_monitor == nullptr) {
			_monitor = new Monitoring();
		}
		
		// Initialize the task monitoring module
		TaskMonitor::initialize();
		
		// Initialize the CPU monitoring module
		CPUMonitor::initialize();
		
		#if CHRONO_ARCH
			// Stop measuring time and compute the tick conversion rate
			TickConversionUpdater::finishUpdate();
		#endif
	}
}

void Monitoring::shutdown()
{
	if (_enabled) {
		#if CHRONO_ARCH
			// Destroy the tick conversion updater service
			TickConversionUpdater::shutdown();
		#endif
		
		// Display monitoring statistics
		displayStatistics();
		
		// Propagate shutdown to the CPU monitoring module
		CPUMonitor::shutdown();
		
		// Propagate shutdown to the task monitoring module
		TaskMonitor::shutdown();
		
		// Destroy the monitoring module
		if (_monitor != nullptr) {
			delete _monitor;
		}
		
		_enabled.setValue(false);
	}
}

void Monitoring::displayStatistics()
{
	if (_enabled && _verbose) {
		// Try opening the output file
		std::ios_base::openmode openMode = std::ios::out;
		std::ofstream output(_outputFile.getValue(), openMode);
		FatalErrorHandler::warnIf(
			!output.is_open(),
			"Could not create or open the verbose file: ",
			_outputFile.getValue(),
			". Using standard output."
		);
		
		// Retrieve statistics from every module
		std::stringstream outputStream;
		CPUMonitor::displayStatistics(outputStream);
		TaskMonitor::displayStatistics(outputStream);
		
		if (output.is_open()) {
			// Output into the file and close it
			output << outputStream.str();
			output.close();
		}
		else {
			std::cout << outputStream.str();
		}
	}
}

bool Monitoring::isEnabled()
{
	return _enabled;
}


//    TASKS    //

void Monitoring::taskCreated(Task *task)
{
	if (_enabled) {
		assert(task != nullptr);
		
		// Retrieve information about the task
		TaskStatistics  *parentStatistics  = (task->getParent() != nullptr ? task->getParent()->getTaskStatistics() : nullptr);
		TaskPredictions *parentPredictions = (task->getParent() != nullptr ? task->getParent()->getTaskPredictions() : nullptr);
		TaskStatistics  *taskStatistics    = task->getTaskStatistics();
		TaskPredictions *taskPredictions   = task->getTaskPredictions();
		const std::string &label = task->getLabel();
		size_t cost = (task->hasCost() ? task->getCost() : DEFAULT_COST);
		
		assert(taskStatistics != nullptr);
		assert(taskPredictions != nullptr);
		
		// Create task statistic structures and predict its execution time
		TaskMonitor::taskCreated(parentStatistics, taskStatistics, parentPredictions, taskPredictions, label, cost);
		TaskMonitor::predictTime(taskPredictions, label, cost);
	}
}

void Monitoring::taskChangedStatus(Task *task, monitoring_task_status_t newStatus, ComputePlace *cpu)
{
	if (_enabled) {
		assert(task != nullptr);
		
		// Start timing for the appropriate stopwatch
		const monitoring_task_status_t oldStatus = TaskMonitor::startTiming(task->getTaskStatistics(), newStatus);
		
		// Update CPU statistics only after a change of status
		if (oldStatus != newStatus) {
			if (cpu != nullptr) {
				// If the task is about to be executed, resume CPU activeness
				if (newStatus == executing_status || newStatus == runtime_status) {
					CPUMonitor::cpuBecomesActive(((CPU *) cpu)->_virtualCPUId);
				}
				// If the task is about to end or block, resume CPU idleness
				else if (newStatus == blocked_status || newStatus == ready_status || newStatus == pending_status) {
					CPUMonitor::cpuBecomesIdle(((CPU *) cpu)->_virtualCPUId);
				}
			}
		}
	}
}

void Monitoring::taskFinished(Task *task)
{
	if (_enabled) {
		assert(task != nullptr);
		
		// Mark task as completely executed
		TaskMonitor::stopTiming(task->getTaskStatistics(), task->getTaskPredictions());
	}
}


//    THREADS    //

void Monitoring::initializeThread()
{
	if (_enabled) {
		// Empty thread API
	}
}

void Monitoring::shutdownThread()
{
	if (_enabled) {
		// Empty thread API
	}
}

