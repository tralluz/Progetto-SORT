#ifndef EXECUTIVE_H
#define EXECUTIVE_H

#include <vector>
#include <functional>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>

class Executive
{
	public:
		Executive(size_t num_tasks, unsigned int frame_length, unsigned int unit_duration = 10);
		void set_periodic_task(size_t task_id, std::function<void()> periodic_task, unsigned int wcet);
		void set_aperiodic_task(std::function<void()> aperiodic_task, unsigned int wcet);
		void add_frame(std::vector<size_t> frame);
		void start();
		void wait();
		void ap_task_request();

	private:
		struct task_data
		{
			std::function<void()> function;
			unsigned int wcet;
			std::thread thread;
			std::mutex mtx;
			std::condition_variable cv;
			std::condition_variable cv_done;
			bool run = false;
			bool done = false;
			//bool ap_task_requested_this_frame = false; // Serve per bloccare richieste multiple nello stesso frame
		};

		size_t frame_id = 0;
		std::vector<task_data> p_tasks;
		task_data ap_task;
		bool ap_task_set = false;
		std::thread exec_thread;
		std::vector< std::vector<size_t> > frames;
		const unsigned int frame_length;
		const std::chrono::milliseconds unit_time;

		static void task_function(task_data & task);
		void exec_function();
};

#endif
