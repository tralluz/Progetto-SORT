#include <cassert>
#include <iostream>

#define VERBOSE

#include "executive.h"
#include "rt/affinity.h"
#include "rt/priority.h"

/* ------------------------------------------------------------------ */
/*  Costruttore / setup                                               */
/* ------------------------------------------------------------------ */

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
/* ------------------------------------------------------------------ */
/*  Start / Wait                                                      */
/* ------------------------------------------------------------------ */
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
	//aggiunto assegnazione massima di priorità all' executive
	try {
    	rt::set_priority(exec_thread, rt::priority::rt_max);
		}	 
	catch (const rt::permission_error& e) {
    	std::cerr << "[ERROR] Impossibile impostare la priorità dell'executive: " << e.what() << std::endl;
		}
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
/* ------------------------------------------------------------------ */
/*  Richiesta asincrona AP task                                       */
/* ------------------------------------------------------------------ */
void Executive::ap_task_request()
{
	//deposita solo il flag sotto mutex
	std::lock_guard<std::mutex> lock(ap_task.mtx);
    if (ap_task.state == TaskState::IDLE ||
        ap_task.state == TaskState::DONE)
        ap_task_requested_this_frame = true;
    //Vecchio codice di ap_task_request
	/* if (ap_task_requested_this_frame) {
		std::cerr << "[ERROR] Il task aperiodico è già stato richiesto in questo frame" << std::endl;
		return;
	}

	if (ap_task.state == TaskState::READY || ap_task.state == TaskState::RUNNING) {
		std::cerr << "[ERROR] Il task aperiodico è già in esecuzione o richiesto" << std::endl;
		return;
	} */

	/* ap_task.state = TaskState::READY;
	ap_task.cv.notify_one(); */
	
}
/* ------------------------------------------------------------------ */
/*  Funzione dei singoli task                                         */
/* ------------------------------------------------------------------ */
void Executive::task_function(Executive::task_data & task)
{
	while (true) {
		std::unique_lock<std::mutex> lock(task.mtx);
		task.cv.wait(lock, [&task]() { return task.state == TaskState::READY; });
		task.state = TaskState::RUNNING;
		lock.unlock();

		task.function();

		lock.lock();
		task.state = TaskState::DONE;
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

	    /* ------------------------------------------------------------------
         * 1) Legge e azzera la richiesta AP sotto mutex
         * ------------------------------------------------------------------ */
        bool req_this_frame;
        {
            std::lock_guard<std::mutex> lock(ap_task.mtx);
            req_this_frame = ap_task_requested_this_frame;
            ap_task_requested_this_frame = false;
        }

        /* ------------------------------------------------------------------
         * 2) Gestione eventuale task aperiodico
         * ------------------------------------------------------------------ */
        if (req_this_frame) {
            std::unique_lock<std::mutex> lock(ap_task.mtx);

            if (ap_task.state == TaskState::READY ||
                ap_task.state == TaskState::RUNNING)
            {
                std::cerr << "[DEADLINE MISS] AP task ancora in esecuzione "
                             "al nuovo rilascio\n";
                ap_task.state = TaskState::DONE;
                try {
                    rt::set_priority(ap_task.thread, rt::priority::rt_min);
                } catch (const rt::permission_error& e) {
                    std::cerr << "[ERROR] set_priority AP: "
                              << e.what() << '\n';
                }
            } else {
                ap_task.state = TaskState::READY;
                ap_task.cv.notify_one();          // UNICO notify
            }
        }

        /* ------------------------------------------------------------------
         * 3) Rilascio dei task periodici del frame corrente
         * ------------------------------------------------------------------ */
        auto prio = rt::priority::rt_max - 1; // I task partono da priorità subito sotto l’executive
		for (auto id : frames[frame_id]) {
            auto& task = p_tasks[id];
            std::unique_lock<std::mutex> lock(task.mtx);

            if (task.state == TaskState::IDLE || task.state == TaskState::DONE)
            {
                task.state = TaskState::READY;

				try {
					rt::set_priority(task.thread, prio);
				} catch (const rt::permission_error& e) {
					std::cerr << "[ERROR] set_priority task " << id
					<< ": " << e.what() << '\n';
				}
				
				task.cv.notify_one();
				prio--; //Il task successivo avrà priorità minore
            } else {
                std::cerr << "[WARN] Task " << id
                          << " in stato " << static_cast<int>(task.state)
                          << " al rilascio\n";
            }
        }

        /* ------------------------------------------------------------------
         * 4) Dorme fino all’inizio del prossimo frame 
         * ------------------------------------------------------------------ */
        next_frame_time += frame_length * unit_time;
        std::this_thread::sleep_until(next_frame_time);

        /* ------------------------------------------------------------------
         * 5) Verifica deadline-miss di tutti i task del frame appena chiuso
         * ------------------------------------------------------------------ */
        for (auto id : frames[frame_id]) {
            auto& task = p_tasks[id];
            std::lock_guard<std::mutex> lock(task.mtx);

            if (task.state != TaskState::DONE) {
                std::cerr << "[DEADLINE MISS] Task " << id << '\n';
                try {
                    rt::set_priority(task.thread, rt::priority::rt_min);
                } catch (const rt::permission_error& e) {
                    std::cerr << "[ERROR] set_priority task " << id
                              << ": " << e.what() << '\n';
                }
                task.state = TaskState::DONE;
            }
        }

        if (ap_task_set) {
            std::lock_guard<std::mutex> lock(ap_task.mtx);
            if (ap_task.state == TaskState::READY ||
                ap_task.state == TaskState::RUNNING)
            {
                std::cerr << "[DEADLINE MISS] Task aperiodico\n";
                try {
                    rt::set_priority(ap_task.thread, rt::priority::rt_min);
                } catch (const rt::permission_error& e) {
                    std::cerr << "[ERROR] set_priority AP: "<< e.what() << '\n';
                }
                ap_task.state = TaskState::DONE;
            }
        }

        /* ------------------------------------------------------------------
         * 6) Passa al frame successivo
         * ------------------------------------------------------------------ */
        frame_id = (frame_id + 1) % frames.size();
    }
}