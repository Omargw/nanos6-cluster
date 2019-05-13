#ifndef MONITORING_HPP
#define MONITORING_HPP

#include "CPUMonitor.hpp"
#include "TaskMonitor.hpp"
#include "lowlevel/EnvironmentVariable.hpp"
#include "lowlevel/FatalErrorHandler.hpp"
#include "tasks/Task.hpp"


class Monitoring {

private:
	
	//! Whether monitoring has to be performed or not
	static EnvironmentVariable<bool> _enabled;
	
	//! Whether verbose mode is enabled
	static EnvironmentVariable<bool> _verbose;
	
	//! The file where output must be saved when verbose mode is enabled
	static EnvironmentVariable<std::string> _outputFile;
	
	// The "monitor", singleton instance
	static Monitoring *_monitor;
	
	
private:
	
	inline Monitoring()
	{
	}
	
	
public:
	
	// Delete copy and move constructors/assign operators
	Monitoring(Monitoring const&) = delete;            // Copy construct
	Monitoring(Monitoring&&) = delete;                 // Move construct
	Monitoring& operator=(Monitoring const&) = delete; // Copy assign
	Monitoring& operator=(Monitoring &&) = delete;     // Move assign
	
	
	//    MONITORING    //
	
	//! \brief Initialize monitoring
	static void initialize();
	
	//! \brief Shutdown monitoring
	static void shutdown();
	
	//! \brief Display monitoring statistics
	static void displayStatistics();
	
	//! \brief Whether monitoring is enabled
	static bool isEnabled();
	
	
	//    TASKS    //
	
	//! \brief Gather basic information about a task when it is created
	//! \param task The task to gather information about
	static void taskCreated(Task *task);
	
	//! \brief Propagate monitoring operations after a task has changed its
	//! execution status
	//! \param task The task that's changing status
	//! \param newStatus The new execution status of the task
	//! \param cpu The cpu onto which a thread is running the task
	static void taskChangedStatus(Task *task, monitoring_task_status_t newStatus, ComputePlace *cpu = nullptr);
	
	//! \brief Propagate monitoring operations after a task has finished
	//! \param task The task that has finished
	static void taskFinished(Task *task);
	
	
	//    THREADS    //
	
	//! \brief Propagate monitoring operations when a thread is initialized
	static void initializeThread();
	
	//! \brief Propagate monitoring operations when a thread is shutdown
	static void shutdownThread();
	
};

#endif // MONITORING_HPP
