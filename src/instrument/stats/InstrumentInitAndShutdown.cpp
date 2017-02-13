#include "InstrumentInitAndShutdown.hpp"
#include "InstrumentStats.hpp"
#include "lowlevel/EnvironmentVariable.hpp"
#include <executors/threads/ThreadManager.hpp>
#include "performance/HardwareCounters.hpp"

#include <fstream>


namespace Instrument {
	namespace Stats {
		static void emitTaskInfo(std::ofstream &output, std::string const &name, TaskInfo &taskInfo)
		{
			TaskTimes meanTimes = taskInfo._times / taskInfo._numInstances;
			
			double meanLifetime = meanTimes.getTotal();
			
			output << "STATS\t" << name << " instances\t"
				<< taskInfo._numInstances << std::endl;
			output << "STATS\t" << name << " mean instantiation time\t"
				<< meanTimes._instantiationTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._instantiationTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean pending time\t"
				<< meanTimes._pendingTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._pendingTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean ready time\t"
				<< meanTimes._readyTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._readyTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean execution time\t"
				<< meanTimes._executionTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._executionTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean blocked time\t"
				<< meanTimes._blockedTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._blockedTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean zombie time\t"
				<< meanTimes._zombieTime << "\t" << Timer::getUnits()
				<< "\t" << 100.0 * (double) meanTimes._zombieTime / meanLifetime << "\t%" << std::endl;
			output << "STATS\t" << name << " mean lifetime\t"
				<< meanTimes.getTotal() << "\t" << Timer::getUnits() << std::endl;
			
			for (HardwareCounters::counter_value_t const &counterValue : taskInfo._hardwareCounters[0]) {
				output << "STATS\t" << name << " " << counterValue._name << "\t";
				
				if (counterValue._isInteger) {
					output << counterValue._integerValue;
				} else {
					output << counterValue._floatValue;
				}
				
				if (!counterValue._units.empty()) {
					output << "\t" << counterValue._units;
				}
				
				output << std::endl;
			}
			
		}
	}
	
	
	using namespace Stats;
	
	
	void shutdown()
	{
		_totalTime.stop();
		double totalTime = _totalTime;
		
		HardwareCounters::shutdown();
		
		ThreadInfo accumulatedThreadInfo(false);
		int numThreads = 0;
		for (auto &threadInfo : _threadInfoList) {
			threadInfo->stoppedAt(_totalTime);
			
			accumulatedThreadInfo += *threadInfo;
			numThreads++;
		}
		
		double totalThreadTime = accumulatedThreadInfo._blockedTime;
		totalThreadTime += (double) accumulatedThreadInfo._runningTime;
		double averageThreadTime = totalThreadTime / (double) numThreads;
		
		TaskInfo accumulatedTaskInfo;
		for (auto &taskInfoEntry : accumulatedThreadInfo._perTask) {
			accumulatedTaskInfo += taskInfoEntry.second;
		}
		
		EnvironmentVariable<std::string> _outputFilename("NANOS6_STATS_FILE", "/dev/stderr");
		std::ofstream output(_outputFilename);
		
		output << "STATS\t" << "Total CPUs\t" << ThreadManager::getTotalCPUs() << std::endl;
		output << "STATS\t" << "Total threads\t" << numThreads << std::endl;
		output << "STATS\t" << "Mean threads per CPU\t" << ((double) numThreads) / (double) ThreadManager::getTotalCPUs() << std::endl;
		output << "STATS\t" << "Mean tasks per thread\t" << ((double) accumulatedTaskInfo._numInstances) / (double) numThreads << std::endl;
		output << std::endl;
		output << "STATS\t" << "Mean thread lifetime\t" << 100.0 * averageThreadTime / totalTime << "\t%" << std::endl;
		output << "STATS\t" << "Mean thread running time\t" << 100.0 * ((double) accumulatedThreadInfo._runningTime) / totalThreadTime << "\t%" << std::endl;
		
		if (accumulatedTaskInfo._numInstances > 0) {
			output << std::endl;
			emitTaskInfo(output, "All Tasks", accumulatedTaskInfo);
		}
		
		for (auto &taskInfoEntry : accumulatedThreadInfo._perTask) {
			nanos_task_info const *userSideTaskInfo = taskInfoEntry.first;
			
			assert(userSideTaskInfo != 0);
			std::string name;
			if (userSideTaskInfo->task_label != 0) {
				name = userSideTaskInfo->task_label;
			} else if (userSideTaskInfo->declaration_source != 0) {
				name = userSideTaskInfo->declaration_source;
			} else {
				name = "Unknown task";
			}
			
			output << std::endl;
			emitTaskInfo(output, name, taskInfoEntry.second);
		}
		
		output.close();
	}
}


