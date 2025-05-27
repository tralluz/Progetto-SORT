#include <cassert>
#include <iostream>

#define VERBOSE

#include "executive.h"
#include "rt/affinity.h"
#include "rt/priority.h"

Executive::Executive(size_t num_tasks, unsigned int frame_length, unsigned int unit_duration)
	: p_tasks(num_tasks), frame_length(frame_length), unit_time(unit_duration)
{
	ap_task.done = true;
}

void Executive::set_periodic_task(size_t task_id, std::function<void()> periodic_task, unsigned int wcet)
{
	assert(task_id < p_tasks.size()); // Fallisce in caso di task_id non corretto (fuori range)
	
	p_tasks[task_id].function = periodic_task;
	p_tasks[task_id].wcet = wcet;
}

void Executive::set_aperiodic_task(std::function<void()> aperiodic_task, unsigned int wcet)
{
 	ap_task.function = aperiodic_task;
 	ap_task.wcet = wcet;
	ap_task.is_aperiodic = true;
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

	
	assert(ap_task.function); // Fallisce se set_aperiodic_task() non e' stato invocato
	
	ap_task.thread = std::thread(&Executive::task_function, std::ref(ap_task));
	
	exec_thread = std::thread(&Executive::exec_function, this);
	rt::set_affinity(exec_thread, core0);
}
	
void Executive::wait()
{
	exec_thread.join();

	ap_task.thread.join();
	
	for (auto & pt: p_tasks)
		pt.thread.join();
}

void Executive::ap_task_request()
{
	 // Serve il lock per accedere in modo sicuro
    std::unique_lock<std::mutex> lock(ap_task.mtx);
	if (!ap_task.done) { //controllo se il task aperiodico è ancora attivo quando viene rilasciata una sua nuova istanza
        std::cerr << "[ERROR] Tentativo di rilascio del task aperiodico mentre è ancora in esecuzione" << std::endl;
        return; // Salta il rilascio
    }
    ap_task.run = true;       // Imposto il flag che dice di partire
    ap_task.done = false;    // Resetto il flag di terminazione
    ap_task.cv.notify_one();  // Risveglio il thread del task aperiodico
	
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
		//task.run = false;
		if (!task.is_aperiodic) {
			task.run = false;
		}

		
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

       
        // Calcola lo slack time
        unsigned int sum_wcet = 0;
        for (auto task_id : frames[frame_id]) {
            sum_wcet += p_tasks[task_id].wcet;
        }
        int slack = static_cast<int>(frame_length) - static_cast<int>(sum_wcet);
        std::cout << "[EXECUTIVE] Slack = " << slack << ", task aperiodico richiesto? " << (ap_task.run ? "Sì" : "No") << std::endl;

        auto frame_start = std::chrono::steady_clock::now();

        // === SLACK STEALING ===
        if (slack > 0) {
            std::unique_lock<std::mutex> lock(ap_task.mtx);
            if (ap_task.run && !ap_task.done) {
#ifdef VERBOSE
                std::cout << "[SLACK STEALING] Lancio task aperiodico per " << slack << " unità temporali (" << (slack * unit_time.count()) << " ms)" << std::endl;
#endif
                ap_task.cv.notify_one();

                // Dormi SOLO fino a fine slack!
                lock.unlock();
                std::this_thread::sleep_until(frame_start + slack * unit_time);
                lock.lock();

                // Se non ha finito, segnala deadline miss
                if (!ap_task.done) {
                    std::cerr << "[DEADLINE MISS] Aperiodic task" << std::endl;
                    ap_task.done = true;
                }
                ap_task.run = false;
            } else {
                // Se il task AP non è richiesto, dormi lo slack per sincronizzazione
                lock.unlock();
                std::this_thread::sleep_until(frame_start + slack * unit_time);
                lock.lock();
            }
        }

		//Sveglia tutti i task del frame
		 for (auto task_id : frames[frame_id]) {
            auto &task = p_tasks[task_id];
            std::unique_lock<std::mutex> lock(task.mtx);
            task.run = true;
            task.done = false;
            task.cv.notify_one();
        }
		//attendi che task abbia finito
		for(auto task_id : frames[frame_id]){
			auto &task = p_tasks[task_id];
			std::unique_lock<std::mutex> lock(task.mtx);

			if(!task.cv_done.wait_until(lock, std::chrono::steady_clock::now() + frame_length * unit_time, [&]() {return task.done;}))
			{
                std::cerr << "[DEADLINE MISS] Task " << task_id << std::endl;
				try {
					rt::set_priority(task.thread, rt::priority::rt_min);
				} catch (const rt::permission_error &e) {
					std::cerr << "[ERROR] Impossibile abbassare la priorità: " << e.what() << std::endl;
				}
				
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
		
		/* if (++frame_id == frames.size())
		{
			frame_id = 0;
		} */
	}
}


