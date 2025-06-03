#include <cassert>
#include <iostream>

#define VERBOSE

#include "executive.h"
#include "rt/affinity.h"
#include "rt/priority.h"

Executive::Executive(size_t num_tasks, unsigned int frame_length, unsigned int unit_duration)
	: p_tasks(num_tasks), frame_length(frame_length), unit_time(unit_duration)
{
}

void Executive::set_periodic_task(size_t task_id, std::function<void()> periodic_task, unsigned int wcet)
{
	assert(task_id < p_tasks.size());
	p_tasks[task_id].function = periodic_task;
	p_tasks[task_id].wcet = wcet;
}

void Executive::set_aperiodic_task(std::function<void()> aperiodic_task, unsigned int wcet)
{
	ap_task.function = aperiodic_task;
	ap_task.wcet = wcet;
	ap_task_set = true;
}

void Executive::add_frame(std::vector<size_t> frame)
{
	for (auto & id: frame)
		assert(id < p_tasks.size());

	frames.push_back(frame);
}

void Executive::start()
{
	rt::affinity core0(1);
	for (size_t id = 0; id < p_tasks.size(); ++id)
	{
		assert(p_tasks[id].function);
		p_tasks[id].thread = std::thread(&Executive::task_function, std::ref(p_tasks[id]));
		rt::set_affinity(p_tasks[id].thread, core0);
	}

	if (ap_task_set) {
		ap_task.thread = std::thread(&Executive::task_function, std::ref(ap_task));
		rt::set_affinity(ap_task.thread, core0);
	}

	exec_thread = std::thread(&Executive::exec_function, this);
	rt::set_affinity(exec_thread, core0);
}

void Executive::wait()
{
	exec_thread.join();
	if (ap_task_set) {
		ap_task.thread.join();
	}
	for (auto & pt: p_tasks)
		pt.thread.join();
}

void Executive::ap_task_request()
{
	 std::unique_lock<std::mutex> lock(ap_task.mtx);
    
    if (ap_task_requested_this_frame) {
        std::cerr << "[ERROR] Il task aperiodico è già stato richiesto in questo frame" << std::endl;
        return;
    }

    if (ap_task.run) {
        std::cerr << "[ERROR] Il task aperiodico è già in esecuzione" << std::endl;
        return;
    }

    ap_task.run = true;
    ap_task.done = false;
    ap_task.cv.notify_one();
    ap_task_requested_this_frame = true; //segna che in questo fram è già stato richiesto

}

void Executive::task_function(Executive::task_data & task)
{
	while(true) {
		std::unique_lock<std::mutex> lock(task.mtx);
		task.cv.wait(lock, [&task]() {return task.run;});
		lock.unlock();
		task.function();
		lock.lock();
		task.done = true;
		task.run = false;
		task.cv_done.notify_one();
	}
}

void Executive::exec_function()
{
	rt::affinity core0(1);
	rt::this_thread::set_affinity(core0);
	frame_id = 0;
	auto next_frame_time = std::chrono::steady_clock::now();

	while (true)
	{
#ifdef VERBOSE
		std::cout << "*** Frame n." << frame_id << (frame_id == 0 ? " ******" : "") << std::endl;
#endif
		for (auto task_id : frames[frame_id]) {
			auto &task = p_tasks[task_id];
			std::unique_lock<std::mutex> lock(task.mtx);
			task.run = true;
			task.done = false;
			task.cv.notify_one();
		}

		for(auto task_id : frames[frame_id]){
			auto &task = p_tasks[task_id];
			std::unique_lock<std::mutex> lock(task.mtx);

if(!task.cv_done.wait_until(lock, std::chrono::steady_clock::now() + frame_length * unit_time, [&]() {return task.done;}))
{
    std::cerr << "[DEADLINE MISS] Task " << task_id << std::endl;
    try {
        rt::set_priority(task.thread, rt::priority::rt_min);
    } catch (const rt::permission_error& e) {
        std::cerr << "[ERROR] Impossibile abbassare la priorità: " << e.what() << std::endl;
    }
}
		}
		if (ap_task_set) {
			std::unique_lock<std::mutex> lock(ap_task.mtx);
			if (ap_task.run && !ap_task.done)
			{
				ap_task.cv.notify_one();
				if(!ap_task.cv_done.wait_until(lock, std::chrono::steady_clock::now() + frame_length * unit_time, [&]() { return ap_task.done; })) {
					std::cerr << "[DEADLINE MISS] Task aperiodico" << std::endl;
				}
			}
		}

		next_frame_time += frame_length * unit_time;
		std::this_thread::sleep_until(next_frame_time);
		ap_task_requested_this_frame = false;
		frame_id = (frame_id + 1) % frames.size();
	}
}
