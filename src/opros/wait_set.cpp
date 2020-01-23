#include "wait_set.hpp"

#include <cstring>

#include <utki/exception.hpp>

#if M_OS == M_OS_MACOSX
#	include <sys/time.h>
#endif



using namespace opros;



#if M_OS == M_OS_MACOSX

void wait_set::AddFilter(Waitable& w, int16_t filter){
	struct kevent e;

	EV_SET(&e, w.getHandle(), filter, EV_ADD | EV_RECEIPT, 0, 0, (void*)&w);

	const timespec timeout = {0, 0}; // 0 to make effect of polling, because passing NULL will cause to wait indefinitely.

	int res = kevent(this->queue, &e, 1, &e, 1, &timeout);
	if(res < 0){
		throw std::system_error(errno, std::generic_category(), "wait_set::Add(): AddFilter(): kevent() failed");
	}
	
	ASSERT((e.flags & EV_ERROR) != 0) // EV_ERROR is always returned because of EV_RECEIPT, according to kevent() documentation.
	if(e.data != 0){ // data should be 0 if added successfully
		TRACE(<< "wait_set::Add(): e.data = " << e.data << std::endl)
		throw std::runtime_error("wait_set::Add(): AddFilter(): kevent() failed to add filter");
	}
}



void wait_set::RemoveFilter(Waitable& w, int16_t filter){
	struct kevent e;

	EV_SET(&e, w.getHandle(), filter, EV_DELETE | EV_RECEIPT, 0, 0, 0);

	const timespec timeout = {0, 0}; //0 to make effect of polling, because passing NULL will cause to wait indefinitely.

	int res = kevent(this->queue, &e, 1, &e, 1, &timeout);
	if(res < 0){
		//ignore the failure
		TRACE(<< "wait_set::Remove(): RemoveFilter(): kevent() failed" << std::endl);
	}
	
	ASSERT((e.flags & EV_ERROR) != 0) //EV_ERROR is always returned because of EV_RECEIPT, according to kevent() documentation.
}

#endif



void wait_set::add(Waitable& w, Waitable::EReadinessFlags flagsToWaitFor){
//		TRACE(<< "wait_set::Add(): enter" << std::endl)
	ASSERT(!w.isAdded())

#if M_OS == M_OS_WINDOWS
	ASSERT(this->numWaitables_var <= this->handles.size())
	if(this->numWaitables_var == this->handles.size()){
		throw utki::invalid_state("wait_set::Add(): wait set is full");
	}

	//NOTE: Setting wait flags may throw an exception, so do that before
	//adding object to the array and incrementing number of added objects.
	w.setWaitingEvents(flagsToWaitFor);

	this->handles[this->numWaitables_var] = w.getHandle();
	this->waitables[this->numWaitables_var] = &w;

#elif M_OS == M_OS_LINUX
	epoll_event e;
	e.data.fd = w.getHandle();
	e.data.ptr = &w;
	e.events =
			(std::uint32_t(flagsToWaitFor) & Waitable::READ ? (EPOLLIN | EPOLLPRI) : 0)
			| (std::uint32_t(flagsToWaitFor) & Waitable::WRITE ? EPOLLOUT : 0)
			| (EPOLLERR);
	int res = epoll_ctl(
			this->epollSet,
			EPOLL_CTL_ADD,
			w.getHandle(),
			&e
		);
	if(res < 0){
		TRACE(<< "wait_set::Add(): epoll_ctl() failed. If you are adding socket, please check that is is opened before adding to wait_set." << std::endl)
		throw std::system_error(errno, std::generic_category(), "wait_set::Add(): epoll_ctl() failed");
	}
#elif M_OS == M_OS_MACOSX
	ASSERT(this->numWaitables() <= revents.size() / 2)
	
	if((std::uint32_t(flagsToWaitFor) & Waitable::READ) != 0){
		this->AddFilter(w, EVFILT_READ);
	}
	if((std::uint32_t(flagsToWaitFor) & Waitable::WRITE) != 0){
		this->AddFilter(w, EVFILT_WRITE);
	}
#else
#	error "Unsupported OS"
#endif

	++this->numWaitables_var;

	w.isAdded_var = true;
//		TRACE(<< "wait_set::Add(): exit" << std::endl)
}



void wait_set::change(Waitable& w, Waitable::EReadinessFlags flagsToWaitFor){
	ASSERT(w.isAdded())

#if M_OS == M_OS_WINDOWS
	//check if the Waitable object is added to this wait set
	{
		unsigned i;
		for(i = 0; i < this->numWaitables_var; ++i){
			if(this->waitables[i] == &w){
				break;
			}
		}
		ASSERT(i <= this->numWaitables_var)
		if(i == this->numWaitables_var){
			throw utki::invalid_state("wait_set::Change(): the Waitable is not added to this wait set");
		}
	}

	//set new wait flags
	w.setWaitingEvents(flagsToWaitFor);

#elif M_OS == M_OS_LINUX
	epoll_event e;
	e.data.fd = w.getHandle();
	e.data.ptr = &w;
	e.events =
			(std::uint32_t(flagsToWaitFor) & Waitable::READ ? (EPOLLIN | EPOLLPRI) : 0)
			| (std::uint32_t(flagsToWaitFor) & Waitable::WRITE ? EPOLLOUT : 0)
			| (EPOLLERR);
	int res = epoll_ctl(
			this->epollSet,
			EPOLL_CTL_MOD,
			w.getHandle(),
			&e
		);
	if(res < 0){
		throw std::system_error(errno, std::generic_category(), "wait_set::Change(): epoll_ctl() failed");
	}
#elif M_OS == M_OS_MACOSX
	if((std::uint32_t(flagsToWaitFor) & Waitable::READ) != 0){
		this->AddFilter(w, EVFILT_READ);
	}else{
		this->RemoveFilter(w, EVFILT_READ);
	}
	if((std::uint32_t(flagsToWaitFor) & Waitable::WRITE) != 0){
		this->AddFilter(w, EVFILT_WRITE);
	}else{
		this->RemoveFilter(w, EVFILT_WRITE);
	}
#else
#	error "Unsupported OS"
#endif
}



void wait_set::remove(Waitable& w)noexcept{
	ASSERT(w.isAdded())
	
	ASSERT(this->numWaitables() != 0)

#if M_OS == M_OS_WINDOWS
	//remove object from array
	{
		unsigned i;
		for(i = 0; i < this->numWaitables_var; ++i){
			if(this->waitables[i] == &w){
				break;
			}
		}
		ASSERT(i <= this->numWaitables_var)
		ASSERT_INFO(i != this->numWaitables_var, "wait_set::Remove(): Waitable is not added to wait set")

		unsigned numObjects = this->numWaitables_var - 1;//decrease number of objects before shifting the object handles in the array
		//shift object handles in the array
		for(; i < numObjects; ++i){
			this->handles[i] = this->handles[i + 1];
			this->waitables[i] = this->waitables[i + 1];
		}
	}

	//clear wait flags (disassociate socket and Windows event)
	w.setWaitingEvents(0);

#elif M_OS == M_OS_LINUX
	int res = epoll_ctl(
			this->epollSet,
			EPOLL_CTL_DEL,
			w.getHandle(),
			0
		);
	if(res < 0){
		ASSERT_INFO(false, "wait_set::Remove(): epoll_ctl failed, probably the Waitable was not added to the wait set")
	}
#elif M_OS == M_OS_MACOSX	
	this->RemoveFilter(w, EVFILT_READ);
	this->RemoveFilter(w, EVFILT_WRITE);
#else
#	error "Unsupported OS"
#endif

	--this->numWaitables_var;

	w.isAdded_var = false;
//		TRACE(<< "wait_set::Remove(): completed successfuly" << std::endl)
}



unsigned wait_set::wait(bool waitInfinitly, std::uint32_t timeout, utki::span<Waitable*>* out_events){
	if(this->numWaitables_var == 0){
		throw utki::invalid_state("wait_set::Wait(): no Waitable objects were added to the wait_set, can't perform Wait()");
	}

	if(out_events){
		if(out_events->size() < this->numWaitables_var){
			throw std::invalid_argument("wait_set::Wait(): passed out_events buffer is not large enough to hold all possible triggered objects");
		}
	}

#if M_OS == M_OS_WINDOWS
	DWORD waitTimeout = waitInfinitly ? (INFINITE) : DWORD(timeout);

	DWORD res = WaitForMultipleObjectsEx(
			this->numWaitables_var,
			&*this->handles.begin(),
			FALSE, //do not wait for all objects, wait for at least one
			waitTimeout,
			FALSE
		);

	ASSERT(res != WAIT_IO_COMPLETION)//it is impossible because we supplied FALSE as last parameter to WaitForMultipleObjectsEx()

	//we are not expecting abandoned mutexes
	ASSERT(res < WAIT_ABANDONED_0 || (WAIT_ABANDONED_0 + this->numWaitables_var) <= res)

	if(res == WAIT_FAILED){
		throw std::system_error(GetLastError(), std::generic_category(), "wait_set::Wait(): WaitForMultipleObjectsEx() failed");
	}

	if(res == WAIT_TIMEOUT){
		return 0;
	}

	ASSERT(WAIT_OBJECT_0 <= res && res < (WAIT_OBJECT_0 + this->numWaitables_var ))

	//check for activities
	unsigned numEvents = 0;
	for(unsigned i = 0; i < this->numWaitables_var; ++i){
		if(this->waitables[i]->checkSignaled()){
			if(out_events){
				ASSERT(numEvents < out_events->size())
				out_events->operator[](numEvents) = this->waitables[i];
			}
			++numEvents;
		}else{
			//NOTE: sometimes the event is reported as signaled, but no read/write events indicated.
			//      Don't know why it happens.
//			ASSERT_INFO(i != (res - WAIT_OBJECT_0), "i = " << i << " (res - WAIT_OBJECT_0) = " << (res - WAIT_OBJECT_0) << " waitflags = " << this->waitables[i]->readinessFlags)
		}
	}

	//NOTE: Sometimes the event is reported as signaled, but no actual activity is there.
	//      Don't know why.
//		ASSERT(numEvents > 0)

	return numEvents;

#elif M_OS == M_OS_LINUX
	ASSERT(int(timeout) >= 0)
	int epollTimeout = waitInfinitly ? (-1) : int(timeout);

//		TRACE(<< "going to epoll_wait() with timeout = " << epollTimeout << std::endl)

	int res;

	while(true){
		res = epoll_wait(
				this->epollSet,
				&*this->revents.begin(),
				this->revents.size(),
				epollTimeout
			);

//			TRACE(<< "epoll_wait() returned " << res << std::endl)

		if(res < 0){
			//if interrupted by signal, try waiting again.
			if(errno == EINTR){
				continue;
			}
			throw std::system_error(errno, std::generic_category(), "wait_set::wait(): epoll_wait() failed");
		}
		break;
	};

	ASSERT(unsigned(res) <= this->revents.size())

	unsigned numEvents = 0;
	for(
			epoll_event *e = &*this->revents.begin();
			e < &*this->revents.begin() + res;
			++e
		)
	{
		Waitable* w = static_cast<Waitable*>(e->data.ptr);
		ASSERT(w)
		if((e->events & EPOLLERR) != 0){
			w->setErrorFlag();
		}
		if((e->events & (EPOLLIN | EPOLLPRI)) != 0){
			w->setCanReadFlag();
		}
		if((e->events & EPOLLOUT) != 0){
			w->setCanWriteFlag();
		}
		ASSERT(w->canRead() || w->canWrite() || w->errorCondition())
		if(out_events){
			ASSERT(numEvents < out_events->size())
			out_events->operator[](numEvents) = w;
			++numEvents;
		}
	}

	ASSERT(res >= 0)//NOTE: 'res' can be zero, if no events happened in the specified timeout
	return unsigned(res);
#elif M_OS == M_OS_MACOSX
	struct timespec ts = {
		decltype(timespec::tv_sec)(timeout / 1000), //seconds
		decltype(timespec::tv_nsec)((timeout % 1000) * 1000000) //nanoseconds
	};

	//loop forever
	for(;;){
		int res = kevent(
				this->queue,
				0,
				0,
				&*this->revents.begin(),
				int(this->revents.size()),
				(waitInfinitly) ? 0 : &ts
			);

		if(res < 0){
			if(errno == EINTR){
				continue;
			}
			throw std::system_error(errno, std::generic_category(), "wait_set::wait(): kevent() failed");
		}else if(res == 0){
			return 0; // timeout
		}else if(res > 0){
			unsigned out_i = 0;// index to out_events
			for(unsigned i = 0; i != unsigned(res); ++i){
				struct kevent &e = this->revents[i];
				Waitable *w = reinterpret_cast<Waitable*>(e.udata);
				if(e.filter == EVFILT_WRITE){
					w->setCanWriteFlag();
				}else if(e.filter == EVFILT_READ){
					w->setCanReadFlag();
				}
				
				if((e.flags & EV_ERROR) != 0){
					w->setErrorFlag();
				}
				
				if(out_events){
					//check if Waitable is already added
					unsigned k = 0;
					for(; k != out_i; ++k){
						if(out_events->operator[](k) == w){
							break;
						}
					}
					if(k == out_i){
						ASSERT(out_i < out_events->size())
						out_events->operator[](out_i) = w;
						++out_i;
					}
				}
			}
			return unsigned(res);
		}
	}
#else
#	error "Unsupported OS"
#endif
}