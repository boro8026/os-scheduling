#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;

    uint16_t *coreTimings;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    //create array to the nubmer of cores and initialize to zero
    shared_data->coreTimings = new uint16_t[num_cores];
    for(int i = 0; i < num_cores; i++){
        shared_data->coreTimings[i] = 0;
    }

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            shared_data->ready_queue.push_back(p);
        }
    }

    // Free configuration data from memory
    deleteConfig(config);

    //if the algorithm is SJF, sort it based on time remaining
    if(shared_data->algorithm == ScheduleAlgorithm::SJF){
        shared_data->ready_queue.sort(SjfComparator());
    }
    //if the algorith is pp, sort it based on priority
    else if(shared_data->algorithm == ScheduleAlgorithm::PP){
        shared_data->ready_queue.sort(PpComparator());
    }


    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }

    // Main thread work goes here
    int num_lines = 0;
    uint64_t midpointTime = 0;
    bool midpointTimeGot = false;

    while (!(shared_data->all_terminated))
    {
        // Clear output from previous iteration
        clearOutput(num_lines);

        uint64_t now = currentTime();//Get the time for this iteration

        //This for loop will handle the functionality of processes that need to be started
        for(i=0; i < processes.size(); i++){
            if(processes[i]->getState() == Process::State::NotStarted && processes[i]->getStartTime() <= now - start){
                {
                    const std::lock_guard<std::mutex> lock(shared_data->mutex);
                    processes[i]->setState(Process::State::Ready, now);
                    shared_data->ready_queue.push_back(processes[i]);
                    processes[i]->updateProcess(now);
                    if(shared_data->algorithm == ScheduleAlgorithm::SJF){ //sort the ready queue by remain time
                        shared_data->ready_queue.sort(SjfComparator());
                    }
                    else if(shared_data->algorithm == ScheduleAlgorithm::PP){ //sort ready queue by priority
                        shared_data->ready_queue.sort(PpComparator());
                        for(int j = 0; j < processes.size(); j++){
                            //if a new process has a higher priority than a process on a CPU, interrupt it
                            if(processes[j]->getState() == Process::State::Running && processes[i]->getPriority() < processes[j]->getPriority()){
                                processes[j]->interrupt();
                            }
                        }
                    }
                }
            }
        }
        //This loop will handle funtionality if a process has finished an IO burst
        for(i=0; i < processes.size(); i++){
            if(processes[i]->getState() == Process::State::IO){
                //If the time has been exceeded
                if((int)now - (int)processes[i]->getBurstStartTime() >= (int)processes[i]->getCurrentBurstTime()){//Time elapsed in IO burst is greater than or equal to the time of the IO burst 
                    {
                        const std::lock_guard<std::mutex> lock(shared_data->mutex);
                        processes[i]->setState(Process::State::Ready, now);
                        shared_data->ready_queue.push_back(processes[i]);
                        processes[i]->updateProcess(now);
                        if(shared_data->algorithm == ScheduleAlgorithm::SJF){ //sort queue if SJF
                            shared_data->ready_queue.sort(SjfComparator());
                        }
                        else if(shared_data->algorithm == ScheduleAlgorithm::PP){ //sort queue if PP
                            for(int j = 0; j < processes.size(); j++){
                                //if the process coming out of IO has a higher priority than a process on the CPU
                                if(processes[j]->getState() == Process::State::Running && processes[i]->getPriority() > processes[j]->getPriority()){
                                    processes[j]->interrupt();
                                }
                            }
                            shared_data->ready_queue.sort(PpComparator());
                        }
                    }
                }
            }
        }

        //This loop will handle time slicing for processes on the CPU, if the scheduling algo is RR
        for(i=0; i < processes.size(); i++){
            if(processes[i]->getState() == Process::State::Running){
                if(shared_data->algorithm == ScheduleAlgorithm::RR && now - processes[i]->getBurstStartTime() >= shared_data->time_slice){
                    processes[i]->interrupt();
                }
            }
        }
        
        //This chunk of code will handle calculating the time it takes for half the prcesses to finish
        //This will be used for the throughput final statistics
        int counter = 0;
        //count how many processes have terminated
        for(i = 0; i < processes.size(); i++){
            if(processes[i]->getState() == Process::State::Terminated){
                counter++;
            }
        }
        //if the count has been reached (for even number of processes)
        if(processes.size() % 2 == 0 && !midpointTimeGot){
            if(counter == processes.size()/2){
                midpointTime = currentTime();
                midpointTimeGot = true;
            }
        }
        //if the count has been reached (for odd number of processes)
        else{
            if(counter == processes.size()/2 + 1 && !midpointTimeGot){
                midpointTime = currentTime();
                midpointTimeGot = true;
            }
        }
        //end of time calcuation

        //This chunk of code will check if all of the processes have terminated
        {
            const std::lock_guard<std::mutex> lock(shared_data->mutex);
            shared_data->all_terminated = true;
            for(int i = 0; i < processes.size(); i++){
                if(processes[i]->getState() != Process::State::Terminated){
                    shared_data->all_terminated = false;
                }
            }
        }

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);

        // sleep 50 ms
        usleep(50000);
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    uint64_t end = currentTime(); //get the end time of the simulation
    std::cout << std::endl;
    //CPU Utilization
    double runningAverage = 0.0;
    for(int i = 0; i < num_cores; i++){
        runningAverage += (double)shared_data->coreTimings[i]/((end - start));
    }
    std::cout << "CPU utilization: " << runningAverage/(double)num_cores << std::endl;
    std::cout << std::endl;

    //Throughput
    if(processes.size() % 2 == 0){ //if there is an even number of processes
        //first half
        std::cout << "First half of processes finished throughput: " << std::endl;
        std::cout << (double)(processes.size()/2) / ((midpointTime - start) / 1000.0) << std::endl;
        //second half
        std::cout << "Second half of processes finished throughput: " << std::endl;
        std::cout << (double)(processes.size()/2) / ((end - start) / 1000.0) << " processes per second" << std::endl;
    }
    else{ //if there is an odd number of processes
        //first half
        std::cout << "First half of processes finished throughput: " << std::endl;
        std::cout << (double)((processes.size()/2) + 1) / ((midpointTime - start) / 1000.0) << " processes per second"<< std::endl;
        //second half
        std::cout << "Second half of processes finished throughput: " << std::endl;
        std::cout << (double)(processes.size()/2) / ((end - start) / 1000.0) << " processes per second" << std::endl;
    }
    std::cout << "Overall Average throughput: " << std::endl;
    std::cout << (double)processes.size() / ((end-start) / 1000.0) << " processes per second" << std::endl;
    std::cout << std::endl;

    //Averages
    double avg_wait=0;
    double avg_turn=0;
    for(int i=0; i < processes.size(); i++){
        avg_turn += processes[i]->getTurnaroundTime();
        avg_wait += processes[i]->getWaitTime();
    }
    std::cout << "Average turnaround time: " << avg_turn/(double)processes.size() << std::endl;
    std::cout << "Average wait time: " << avg_wait/(double)processes.size() << std::endl;

    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    //These values will allow us to keep track of when the CPU core is actually "computing"
    uint16_t startOfComputing;
    uint16_t endOfComputing;
    while(!shared_data->all_terminated){
        Process* front;
        {
            //This chunk will grab a process off the ready queue if one exists
            const std::lock_guard<std::mutex> lock(shared_data->mutex);
            if(shared_data->ready_queue.size() > 0){
                front = shared_data->ready_queue.front();
                shared_data->ready_queue.pop_front();
                front->setState(Process::State::Running,currentTime());
                front->setCpuCore(core_id);
                front->updateProcess(currentTime());
                front->setBurstStartTime(currentTime());
            }
        }
        startOfComputing = currentTime(); //grab the time at the start of "computing"
        while(front != NULL){
            if(currentTime() - front->getBurstStartTime() >= front->getCurrentBurstTime()){ //if CPU burst time has elapsed
                if(front->getCurrentBurst() == front->getNumBursts()-1){ //if this was the processes last CPU burst, terminate
                    front->setState(Process::State::Terminated,currentTime());
                    front->setCpuCore(-1);
                    front->updateProcess(currentTime());
                }
                else{
                    front->setState(Process::State::IO,currentTime());  //else, send process to the IO queue
                    front->setCpuCore(-1);
                    front->updateProcess(currentTime());
                }
                break;
            }
            else if(front->isInterrupted()){   //If the process has been interrupted
                {
                    //send the value back to the ready queue, and handle the interrupt
                    const std::lock_guard<std::mutex> lock(shared_data->mutex);
                    shared_data->ready_queue.push_back(front);
                    front->setState(Process::State::Ready,currentTime());
                    front->setCpuCore(-1);
                    front->updateProcess(currentTime());
                    front->interruptHandled();
                    if(shared_data->algorithm == ScheduleAlgorithm::PP){
                        shared_data->ready_queue.sort(PpComparator());
                    }
                }
                break;
            }
        }
        endOfComputing = currentTime(); //get the end time of "computing"
        shared_data->coreTimings[core_id] += (endOfComputing - startOfComputing); //add the time "computing" to the core_id index
        front = NULL;

        usleep(shared_data->context_switch * 1000.0);
    }
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}





