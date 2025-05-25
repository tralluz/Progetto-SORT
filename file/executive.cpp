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

void Executive::set_periodic_task(size_t task_id, std::function<void()> periodic_task, unsigned int /* wcet */)
{
	assert(task_id < p_tasks.size()); // Fallisce in caso di task_id non corretto (fuori range)
	
	p_tasks[task_id].function = periodic_task;
}
		
void Executive::add_frame(std::vector<size_t> frame)
{
	for (auto & id: frame)
		assert(id < p_tasks.size()); // Fallisce in caso di task_id non corretto (fuori range)
	
	frames.push_back(frame);

}

void Executive::start()
{	
	rt::affinity core0(1);
	for (size_t id = 0; id < p_tasks.size(); ++id)
	{
		assert(p_tasks[id].function); // Fallisce se set_periodic_task() non e' stato invocato per questo id		
		p_tasks[id].thread = std::thread(&Executive::task_function, std::ref(p_tasks[id]));
		 rt::set_affinity(p_tasks[id].thread, core0);
	}
	
	exec_thread = std::thread(&Executive::exec_function, this);
	rt::set_affinity(exec_thread, core0);
}
	
void Executive::wait()
{
	exec_thread.join();
	
	for (auto & pt: p_tasks)
		pt.thread.join();
}

/*Questa funzione è il corpo del thread associato a ogni task. 
Ciascun task (come task0, task1...) ha un suo thread dedicato lanciato in Executive::start()*/
void Executive::task_function(Executive::task_data & task)
{
	while(true) {
		//1. Attende il rilascio del task
		std::unique_lock<std::mutex> lock(task.mtx);
		task.cv.wait(lock, [&task]() {return task.run;});

		//2. Esegue il task (fuori dal lock per evitare blocchi durante l'esecuzione)
		lock.unlock();
		task.function();
		lock.lock();

		//3. Segnala che ha finito
		task.done = true;
		task.run = false;
		
		//4. Notifica all'excecutive che il task è terminato
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
		//Sveglia tutti i task del frame
		for(auto task_id : frames[frame_id]){
			auto &task = p_tasks[task_id];
			std::unique_lock<std::mutex> lock(task.mtx);

			if(!task.cv_done.wait_until(lock, std::chrono::steady_clock::now() + frame_length * unit_time, [&]() {return task.done;}))
			{
                std::cerr << "[DEADLINE MISS] Task " << task_id << std::endl;
            }
		}
		  // 4. Attendi l'inizio del prossimo frame (sleep_until garantisce precisione)
        next_frame_time += frame_length * unit_time;
        std::this_thread::sleep_until(next_frame_time);

        // 5. Passa al prossimo frame in modo ciclico
        frame_id = (frame_id + 1) % frames.size();
		
		/* Rilascio dei task periodici del frame corrente ... */
		/* Attesa fino al prossimo inizio frame ... */
		/* Controllo delle deadline ... */
		
		if (++frame_id == frames.size())
		{
			frame_id = 0;
		}
	}
}


