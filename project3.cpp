#include <iostream>
#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <set>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
using namespace std;

/**************************************************************************************/
int vmemSize; // virtual memory 크기
int pmemSize; // physical memory 크기
int pageSize; // page(frame)의 크기

string schedOption;
string pageOption;
string dirOption;

struct Entry { // page table entry에 대한 정보 모음
	int aid; // aid = 0: 테이블에 할당되지 않은 상태
	int fIndex; // page와 mapping되는 frame
	bool valid; // valid bit이 1인가
};

struct Process { // 프로세스의 정보
	string fileName; // 파일 이름
	vector<pair<int, int>> code;
	int line; // 다음에 실행할 코드줄, 0-based
	// running process인 경우: RR방식에서 time quantum이 만료되는 시간
	// sleep process인 경우: 깨어나는 시간
	int endTime;
	// 아래 네 변수는 SJF방식에서 쓰임
	double S; // 이번의 expected cpu burst
	int T; // 이번에 cpu에서 수행된 시간
	int startTime; // running프로세스가 실행되기 시작한 시간
	int N; // 도중에 block되어 cpu burst가 초기화된 횟수
	vector<Entry> table; // page table
	bool busy; // busy waiting중인가
	
	// 생성자 정의
	Process() {} // default constructor
	Process(const string& _fileName) {
		fileName = _fileName;
		string filePath = dirOption + fileName; // 파일이 위치한 경로
		FILE* in = fopen(filePath.c_str(), "r"); // 파일 읽기 시작

		int numCode;
		fscanf(in, "%d", &numCode);
		code.resize(numCode);

		for (int i = 0; i < numCode; ++i)
			fscanf(in, "%d%d", &code[i].first, &code[i].second);
		fclose(in); // 파일 읽기 종료

		line = 0;
		table.resize(vmemSize / pageSize); // 테이블 사이즈 = 페이지 총 개수
		for (int i = 0; i < table.size(); ++i) {
			table[i].aid = 0; // 테이블에 할당된 페이지가 없다
			table[i].fIndex = -1; // 매핑되는 프레임이 없다
			table[i].valid = false; // 매핑 정보가 옮지 않다
		}
		if (schedOption[0] == 's') { // sjf
			S = 5; T = 0; N = 0;
		}
		busy = false;
	}
};
struct Infor { // AID묶음에 대한 정보 모음
	// 속한 프로세스 번호, 시작 페이지 번호, 페이지 개수
	int pid, pageIdx, pageCnt;
	// 아래 정보들은 page valid bit이 1일때만 유효
	// 시작 프레임 번호, 프레임 개수
	int frameIdx, frameCnt;
	// 페이지 교체 알고리즘에 쓰일 정보들
	int allocTime; // 물리 메모리에 새로 올라온 시점
	int accessTime; // 최근에 사용된 시점
	int refBit; // reference bit
	int refByte; // referece byte
	int refCnt; // 참조된 횟수
};

const int MAXPROCESS = 102;
int procCnt = 0; // 다음에 할당될 PID값
int curCycle = 0; // 현재 CPU cycle

// (io작업을 하는 시기, PID)의 모음
// ioInput의 뒤의 원소일수록 시기가 빠름
vector<pair<int, int>> ioInput;
// (프로세스를 생성하는 시기, 파일 이름)의 모음
// exeInput의 뒤의 원소일수록 시기가 빠름
vector<pair<int, string>> exeInput;
// scheduler, memory정보 출력 용도
FILE *sout, *mout;

// proc[i] = (pid=i)인 프로세스의 정보
Process proc[MAXPROCESS];
// ready queue에 있는 프로세스의 pid모음 (뒤일수록 나중에 들어온 것)
vector<int> readyQ;
int running = -1; // cpu에 할당된 프로세스의 pid, 없을시 -1
// sleep중인 프로세스의 pid모음 (뒤일수록 나중에 들어온 것)
vector<int> sleeping;
// io wait중인 프로세스의 pid모음 (뒤일수록 나중에 들어온 것)
vector<int> ioWait;
// future[i] = i번째 cycle에 수행되는 (op, arg)
// optimal 알고리즘을 위해서만 쓰인다
vector<pair<int, int>> future;

// phys[i] = 물리 메모리의 i번 frame에 해당하는 AID
// AID=0이면 free frame임을 뜻한다
vector<int> phys;
// bundle[i] = (AID=i)인 묶음의 정보. bundle[0]는 정의되지 않는다.
// page->frame정보는 해당 page의 valid bit이 1일때만 유효하다.
vector<Infor> bundle;
// key가 locked안에 존재하면 key번 변수가 lock되었다는 뜻
set<int> locked;
int pagefault;
int accessCnt; // 총 메모리 access 시도 횟수
int instCnt; // 지금까지 실행한 명령어 줄 수

/**************************************************************************************/
// 물리 메모리에 있는 aid들을 오름차순으로 정렬하여 반환
vector<int> getPhysAid() {
	vector<int> ret;
	// chk[i] = (aid=i)가 ret안에 있는가
	vector<bool> chk(bundle.size());

	for (int i = 0; i < phys.size(); ++i) {
		int aid = phys[i];
		if (aid == 0 || chk[aid]) continue;
		ret.push_back(aid);
		chk[aid] = true;
	}
	sort(ret.begin(), ret.end());
	return ret;
}

// 종료된 pid번 프로세스의 메모리를 반환한다
void terminateProc(int pid) {
	for (int i = 0; i < phys.size(); ++i) {
		int aid = phys[i];
		if (bundle[aid].pid == pid)
			phys[i] = 0;
	}
}

// 물리 메모리(phys)에 크기 x인 프레임묶음을 할당할 수 있는 공간의 시작위치를 반환
// 공간이 없다면 -1 반환
int getAllocPos(int x) {
	vector<bool> used(phys.size());
	for (int i = 0; i < phys.size(); ++i)
		if (phys[i] != 0)
			used[i] = true;

	// space = (빈 공간의 크기, 시작 위치)의 모음
	vector<pair<int, int>> space;
	for (int sz = used.size(); sz >= x; sz >>= 1) {
		// 크기가 sz인 연속된 빈 공간을 찾는다
		// 단, 버디 시스템에 따라 시작 위치는 sz의 배수여야함
		for (int st = used.size()-sz; st >= 0; st -= sz) {
			bool isEmpty = true;
			for (int i = st; i < st+sz; ++i)
				if (used[i])
					isEmpty = false;
			if (!isEmpty) continue;

			for (int i = st; i < st+sz; ++i)
				used[i] = true;
			// space는 공간 크기가 큰 순 -> 크기가 같을 경우
			// 시작 위치가 큰 순으로 정렬된 상태를 유지한다
			space.push_back(make_pair(sz, st));
		}
	}
	if (space.empty()) return -1; // 크기가 x이상인 공간이 없음
	return space.back().second;	
}

// 페이지 교체 알고리즘을 수행하여 victim이 되는 aid를 반환한다
// 물리 메모리에 빈 공간이 없음을 가정한다
int getVictim() {
	vector<int> v = getPhysAid(); // 물리 메모리에 있는 aid모음
	if (pageOption == "fifo") {
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i)
			if (bundle[victim].allocTime > bundle[v[i]].allocTime)
				victim = v[i];
		return victim;
	}
	else if (pageOption == "lru") {
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i)
			if (bundle[victim].accessTime > bundle[v[i]].accessTime)
				victim = v[i];
		return victim;
	}
	else if (pageOption == "lru-sampled") {
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i) {
			int val1 = 256*bundle[victim].refBit + bundle[victim].refByte;
			int val2 = 256*bundle[v[i]].refBit + bundle[v[i]].refByte;
			if (val1 > val2) victim = v[i];
		}
		return victim;
	}
	else if (pageOption == "lfu") {
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i)
			if (bundle[victim].refCnt > bundle[v[i]].refCnt)
				victim = v[i];
		return victim;
	}
	else if (pageOption == "mfu") {
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i)
			if (bundle[victim].refCnt < bundle[v[i]].refCnt)
				victim = v[i];
		return victim;
	}
	else { // optimal
		// 앞으로 수행할 (op, arg)순서가 future에 저장되어 있음
		// 현재 사이클 이후로 가장 오래 access를 안하는 aid를 고름
		const int INF = 1e9;
		// timeChk[i] = 앞으로 (aid=i)에 최초로 접근하는 시간
		vector<int> timeChk(bundle.size(), INF);
		for (int i = curCycle+1; i < future.size(); ++i) {
			int op = future[i].first;
			int arg = future[i].second;
			if (op == 1 && timeChk[arg] == INF)
				timeChk[arg] = i;
		}
		int victim = v[0];
		for (int i = 1; i < v.size(); ++i)
			if (timeChk[victim] < timeChk[v[i]])
				victim = v[i];
		return victim;
	}
}

/**************************************************************************************/
// Sleep된 프로세스의 종료 여부 검사
void checkSleep() {
	vector<int> nextSleep; // 이번에 sleep이 종료되지 않은 pid모음
	for (int i = 0; i < sleeping.size(); ++i) {
		int pid = sleeping[i];
		if (proc[pid].endTime != curCycle)
			nextSleep.push_back(pid);
		else { // 깨어난 프로세스는 즉시 ready queue에 삽입
			readyQ.push_back(pid);
		}
	}
	sleeping = nextSleep;
}

// Input으로 주어진 IO작업의 시행
void operation_IO() {
	// ioInput.back() = (가장 먼저 진행될 io작업의 시기, 해당 pid)
	while (!ioInput.empty() && ioInput.back().first == curCycle) {
		int pid = ioInput.back().second;
		// it = ioWait상에서 pid의 위치
		auto it = find(ioWait.begin(), ioWait.end(), pid);
		// pid번 프로세스가 io wait중인 경우 ready queue에 삽입
		if (it != ioWait.end()) {
			ioWait.erase(it);
			readyQ.push_back(pid);
		}
		ioInput.pop_back();
	}
}

// Input으로 주어진 Process 생성 작업의 시행
void operation_CP() {
	// exeInput.back() = (가장 먼저 진행될 생성작업 시기, 파일 이름)
	while (!exeInput.empty() && exeInput.back().first == curCycle) {
		string fileName = exeInput.back().second;
		int pid = procCnt++; // 생성될 프로세스의 pid
		proc[pid] = Process(fileName);
		// 생성된 프로세스는 ready queue뒤에 삽입
		readyQ.push_back(pid);
		exeInput.pop_back();
	}
}

// 스케줄링 알고리즘에 따라 프로세스를 채택하여 cpu(running)에 할당
// 프로세스를 ready queue에서 새로 가져왔다면 true 반환
bool dispatch() {
	if (schedOption == "fcfs") {
		// 어떤 프로세스가 실행중이거나 채택할 것이 없는 경우 그대로 유지
		if (running != -1 || readyQ.empty()) return false;
		running = readyQ.front(); // 가장 먼저 큐에 들어온 pid채택
		readyQ.erase(readyQ.begin());
		return true;
	}
	else if (schedOption == "rr") {
		if (running != -1) { // 실행중인 프로세스가 있는 경우
			// time quantum이 만료되지 않았으므로 계속 사용
			return false;
		}
		// 가장 먼저 큐에 들어온 pid채택
		if (readyQ.empty()) return false;
		running = readyQ.front();
		proc[running].endTime = curCycle+9; // time quantum 부여
		readyQ.erase(readyQ.begin());
		return true;
	}
	else { // sjf
		// ready queue의 원소들 모두 새로운 queue에 순서대로 들어온다고 생각한다
		vector<int> nreadyQ;
		bool scheduled = false;

		for (int i = 0; i < readyQ.size(); ++i) {
			nreadyQ.push_back(readyQ[i]);
			// nreadyQ의 프로세스가 running프로세스를 선점할 수 있는지 본다
			int pos = 0; // nreadyQ에서 남은 cpu burst가 가장 작은 위치
			
			for (int j = 1; j < nreadyQ.size(); ++j) {
				int a = proc[nreadyQ[j]].S - proc[nreadyQ[j]].T;
				int b = proc[nreadyQ[pos]].S - proc[nreadyQ[pos]].T;
				if (a < b) pos = j;
			}
			// pid번 프로세스가 cpu를 선점할 경우
			int pid = nreadyQ[pos];
			if (running == -1 || proc[pid].S - proc[pid].T < 
				proc[running].S - proc[running].T) {
				// running중이였던 프로세스가 있으면 큐에 넣는다
				if (running != -1) {
					// pos에 영향을 미치지 않음
					nreadyQ.push_back(running);
					running = -1;
				}
				// pid번 프로세스를 running에 넣기
				running = pid;
				nreadyQ.erase(nreadyQ.begin() + pos);
				proc[running].startTime = curCycle;
				scheduled = true;
			}
		}
		readyQ = nreadyQ;
		return scheduled;
	}
}

// 해당 Cycle에 수행할 Process의 명령을 실행
// 이번 명령 실행으로 프로세스가 종료되었다면 그 pid를 반환, 아니면 -1반환
int executeCode() {
	fprintf(sout, "Running Process: ");
	fprintf(mout, "[%d Cycle] Input : ", curCycle);
	if (running == -1) {
		fprintf(sout, "None\n");
		fprintf(mout, "Function [NO-OP]\n");
		return -1;
	}
	int op = proc[running].code[proc[running].line].first;
	int arg = proc[running].code[proc[running].line].second;
	fprintf(sout, "Process#%d running code %s line %d(op %d, arg %d)\n",
		running, proc[running].fileName.c_str(), proc[running].line+1, op, arg);

	if (!proc[running].busy) ++instCnt;
	++proc[running].line;
	if (schedOption[0] == 's')
		++proc[running].T;

	if (op == 0) { // Memory Allocation
		// arg = 할당할 page개수
		int aid = bundle.size(); // 이번에 할당될 AID값
		fprintf(mout, "Pid [%d] Function [ALLOCATION] Alloc ID [%d] Page Num[%d]\n",
			running, aid, arg); 
		// running프로세스의 page table에서 arg개의 연속된 빈 공간 찾기
		vector<Entry>& table = proc[running].table;
		int l = 0; // 연속된 빈 공간의 시작 인덱스가 될 것임
		
		while (l < table.size()) {
			while (l < table.size() && table[l].aid != 0) ++l;
			int r = l;
			while (r < table.size() && table[r].aid == 0) ++r;
			// [l, r)의 연속된 빈 공간을 찾음
			if (arg <= r-l) break; // 충분한 빈 공간이면 탐색 종료
			l = r;
		}
		// [l, l+arg)의 entry에 페이지를 할당
		for (int i = l; i < l+arg; ++i)
			table[i].aid = aid; // fIndex=-1, valid=false

		// 이번에 생성된 AID묶음의 정보를 bundle에 추가
		Infor add = Infor();
		add.pid = running;
		add.pageIdx = l;
		add.pageCnt = arg;
		bundle.push_back(add);
	}
	else if (op == 1) { // Memory Access
		++accessCnt;
		// arg = 접근 요청하는 Allocation ID
		int pid = bundle[arg].pid;
		int pageIdx = bundle[arg].pageIdx;
		int pageCnt = bundle[arg].pageCnt;
		fprintf(mout, "Pid [%d] Function [ACCESS] Alloc ID [%d] Page Num[%d]\n",
			running, arg, pageCnt);

		if (!proc[pid].table[pageIdx].valid) {
			int frameCnt = 1;
			while (frameCnt < pageCnt) frameCnt <<= 1;

			// 물리 메모리에 할당할 공간의 시작 위치
			int frameIdx = getAllocPos(frameCnt);
			if (frameIdx != -1) ++pagefault;

			while (frameIdx == -1) { // 공간이 없는 경우
				int victim = getVictim();
				++pagefault;
				// aid=victim을 물리 메모리에서 제거
				for (int i = 0; i < phys.size(); ++i)
					if (phys[i] == victim)
						phys[i] = 0;

				// victim에 있는 page의 table 업데이트
				int vpid = bundle[victim].pid;
				for (int i = 0; i < bundle[victim].pageCnt; ++i)
					proc[vpid].table[bundle[victim].pageIdx + i].valid = false;
				
				frameIdx = getAllocPos(frameCnt);
			}
			// 할당할 공간을 찾았음
			// aid=arg를 물리 메모리에 할당
			for (int i = 0; i < frameCnt; ++i)
				phys[frameIdx + i] = arg;
			// page table 업데이트
			for (int i = 0; i < pageCnt; ++i) {
				proc[pid].table[pageIdx + i].fIndex = frameIdx + i;
				proc[pid].table[pageIdx + i].valid = true;
			}
			// aid=arg에 대한 정보 업데이트
			bundle[arg].frameIdx = frameIdx;
			bundle[arg].frameCnt = frameCnt;
			bundle[arg].allocTime = curCycle;
			bundle[arg].accessTime = 0;
			bundle[arg].refBit = bundle[arg].refByte = bundle[arg].refCnt = 0;
		}
		// access로 인한 정보 업데이트
		bundle[arg].accessTime = curCycle;
		bundle[arg].refBit = 1;
		bundle[arg].refCnt += 1;
	}
	else if (op == 2) { // Memory Release
		// arg = 해제 요청하는 allocation id
		fprintf(mout, "Pid [%d] Function [RELEASE] Alloc ID [%d] Page Num[%d]\n",
			running, arg, bundle[arg].pageCnt);
		// 물리 메모리에서 해제
		for (int i = 0; i < phys.size(); ++i)
			if (phys[i] == arg)
				phys[i] = 0;
		// 가상 메모리에서 해제
		vector<Entry>& table = proc[bundle[arg].pid].table;
		for (int i = 0; i < bundle[arg].pageCnt; ++i) {
			Entry& e = table[bundle[arg].pageIdx + i];
			e.aid = 0;
			e.fIndex = -1;
			e.valid = false;
		}
	}
	else if (op == 3) { // Non-memory instruction
		fprintf(mout, "Pid [%d] Function [NONMEMORY]\n", running); 
	}
	else if (op == 4 || op == 5) { // block되는 경우
		int pid = running;
		running = -1;

		if (op == 4) fprintf(mout, "Pid [%d] Function [SLEEP]\n", pid);
		else fprintf(mout, "Pid [%d] Function [IOWAIT]\n", pid); 

		// sleep, io wait이 마지막 명령이 아닌 경우에만 수행
		if (proc[pid].line < proc[pid].code.size()) {
			// pid번 프로세스는 곧 block되는데, 이에 대한 정보를 업데이트
			if (schedOption[0] == 's') { // sjf
				proc[pid].N += 1;
				// block이 끝난 후의 expected cpu burst를 지금 계산해놓는다
				if (proc[pid].N == 1) 
					proc[pid].S = proc[pid].T;
				else {
					// sjf-simple or sjf-exponential
					double a = schedOption[4] == 's' ? 1/(double)proc[pid].N : 0.6;
					proc[pid].S = a*proc[pid].T + (1-a)*proc[pid].S;
				}
				proc[pid].T = 0;
			}
			// op에 따라 알맞은 block 리스트에 넣기
			if (op == 4) { // Sleep
				// arg = 잠들 cycle의 수
				proc[pid].endTime = curCycle + arg;
				sleeping.push_back(pid);
			}
			else ioWait.push_back(pid); // IO Wait
		}
		else return pid; // 마지막 명령인 경우에 block되지 않고 바로 종료한다
	}
	else if (op == 6) { // Lock
		fprintf(mout, "Pid [%d] Function [LOCK]\n", running); 
		// arg = lock할 변수의 ID
		// 다른 프로세스가 lock을 잡고 있는 경우
		if (locked.count(arg)) {
			// busy waiting되어 현재 명령어줄에 머무르기
			proc[running].busy = true;
			--proc[running].line;
		}
		else {
			locked.insert(arg);
			proc[running].busy = false;
		}
	}
	else { // Unlock
		fprintf(mout, "Pid [%d] Function [UNLOCK]\n", running); 
		// arg = unlock할 변수의 ID
		locked.erase(arg);
	}
	// 명령어 수행 직후에 정보들 업데이트
	int term = -1; // 이번에 종료된 프로세스의 pid
	if (running != -1 && proc[running].line == proc[running].code.size()) {
		// 프로세스 종료
		term = running;
		running = -1;
	}
	// time quantum이 다 된 경우
	if (schedOption == "rr" && running != -1 && proc[running].endTime == curCycle) {
		readyQ.push_back(running);
		running = -1;
	}
	// time interval이 다 된 경우
	if (instCnt % 8 == 0) { 
		// reference byte 업데이트
		vector<int> v = getPhysAid(); // 물리 메모리에 있는 aid모음
		for (int i = 0; i < v.size(); ++i) {
			int aid = v[i];
			bundle[aid].refByte >>= 1;
			bundle[aid].refByte += 128 * bundle[aid].refBit;
			bundle[aid].refBit = 0;
		}
	}
	return term;
}

/**************************************************************************************/
int main(int argc, char* argv[]) {
	// 옵션 입력받기
	schedOption = "fcfs"; // default
	pageOption = "fifo"; // default
	dirOption = argv[0]; // "(dir path)/project3"
	dirOption = dirOption.substr(0, dirOption.size()-8); // default

	for (int i = 1; i < argc; ++i) {
		string option = argv[i];
		if (option[1] == 's') // "-sched=schedOption"
			schedOption = option.substr(7);
		else if (option[1] == 'p') // "-page=pageOption"
			pageOption = option.substr(6);
		else { // "-dir=dirOption"
			dirOption = option.substr(5);
			if (dirOption.back() != '/')
				dirOption += '/';
		}
	}
	// "input"파일 읽기 시작
	FILE* in = fopen((dirOption+"input").c_str(), "r");
	int totalEventNum;
	fscanf(in, "%d%d%d%d", &totalEventNum, &vmemSize, &pmemSize, &pageSize);
		
	while (totalEventNum--) {
		int input1;
		string input2;
		char tmp[22];
		
		fscanf(in, "%d%s", &input1, tmp);
		input2 = tmp;

		if (input2 == "INPUT") { // I/O 작업 코드
			int input3;
			fscanf(in, "%d", &input3);
			ioInput.push_back(make_pair(input1, input3));
		}
		else { // 프로세스 생성 코드
			exeInput.push_back(make_pair(input1, input2));
		}
	}
	// 시기 순을 내림차순으로 만들기 위해 원소를 뒤집는다
	reverse(ioInput.begin(), ioInput.end());
	reverse(exeInput.begin(), exeInput.end());
	// "input"파일 읽기 종료
	fclose(in);

	// 전역 변수 크기 조정
	phys.resize(pmemSize / pageSize); // 프레임 개수
	// bundle에는 더미 원소 AID=0 묶음에 대한 정보 추가
	bundle.push_back(Infor());

 	// optimal 알고리즘의 경우 스케줄링 순서를 미리 알아낼 필요가 있다
	if (pageOption == "optimal") {
		// 내부적으로 project3을 한번 더 호출하여 순서를 알아낸다
		// 이때 페이지 교체 알고리즘은 default로 한다 (결과에 영향을 안미침)
		int realPid = fork();
		if (realPid == 0) {
			string dirStr = "-dir=" + dirOption;
			string schedStr = "-sched=" + schedOption;
			char* args[] = 	{argv[0], (char *)dirStr.c_str(), 
				(char *)schedStr.c_str(), NULL};
			execvp(args[0], args);
			exit(0);
		}
		// 자식 프로세스가 끝날 때까지 기다리기
		int status; wait(&status);
		// 자식 프로세스가 내놓은 스케줄링 결과를 future에 저장함
		ifstream in(dirOption + "scheduler.txt");
		future.push_back(make_pair(-1, -1));

		while (in.peek() != EOF) {
			string buf;
			getline(in, buf);
			// buf를 파싱하여 정보를 추출한다
			if (buf.size() > 3 && buf[3] == 'n') {
				// buf = "Running Process: ~~"
				if (buf[17] == 'N')
					future.push_back(make_pair(-1, -1));
				else {
					int l = 0;
					while (buf[l] != '(') ++l;
					l += 4;
					int op = buf[l]-'0';

					l += 7;
					int r = l;
					while (buf[r] != ')') ++r;
					int arg = stoi(buf.substr(l, r-l));
					future.push_back(make_pair(op, arg));
				}
			}
		}
		in.close();
		// 임시 파일 삭제
		remove((dirOption + "scheduler.txt").c_str());
		remove((dirOption + "memory.txt").c_str());
	}
	// "scheduler.txt", "memory.txt" 정보 출력 준비
	sout = fopen((dirOption + "scheduler.txt").c_str(), "w");
	mout = fopen((dirOption + "memory.txt").c_str(), "w");

	while (++curCycle) {
		// 0. 프로세스가 모두 종료한 경우 작업 종료
		if (running == -1 && readyQ.empty() && sleeping.empty() &&
			ioWait.empty() && ioInput.empty() && exeInput.empty()) break;
		// 1. Sleep된 프로세스의 종료 여부 검사
		checkSleep();
		// 2. Input으로 주어진 IO작업의 시행
		operation_IO();
		// 3. Input으로 주어진 Process 생성 작업의 시행
		operation_CP();

		// 4. 스케줄러에 의해 이번에 실행할 프로세스(running) 갱신
		bool scheduled = dispatch();
		fprintf(sout, "[%d Cycle] Scheduled Process: ", curCycle);
		if (scheduled) 
			fprintf(sout, "%d %s\n", running, proc[running].fileName.c_str());
		else fprintf(sout, "None\n");

		// 5. 해당 Cycle에 수행할 Process의 명령을 실행
		int term = executeCode();

		// 6. scheduler.txt와 memory.txt의 남은 정보 출력
		fprintf(sout, "RunQueue: ");
		if (readyQ.empty())
			fprintf(sout, "Empty");
		else {
			for (int i = 0; i < readyQ.size(); ++i) {
				int pid = readyQ[i];
				fprintf(sout, "%d(%s) ", pid, proc[pid].fileName.c_str());
			}
		}
		fprintf(sout, "\n");

		fprintf(sout, "SleepList: ");
		if (sleeping.empty())
			fprintf(sout, "Empty");
		else {
			for (int i = 0; i < sleeping.size(); ++i) {
				int pid = sleeping[i];
				fprintf(sout, "%d(%s) ", pid, proc[pid].fileName.c_str());
			}
		}
		fprintf(sout, "\n");

		fprintf(sout, "IOWait List: ");
		if (ioWait.empty())
			fprintf(sout, "Empty");
		else {
			for (int i = 0; i < ioWait.size(); ++i) {
				int pid = ioWait[i];
				fprintf(sout, "%d(%s) ", pid, proc[pid].fileName.c_str());
			}
		}
		fprintf(sout, "\n\n");

		fprintf(mout, "%-30s", ">> Physical Memory : ");
		for (int i = 0; i < phys.size(); i += 4) {
			fprintf(mout, "|");
			for (int j = i; j < i+4; ++j) {
				if (phys[j] == 0) fprintf(mout, "-");
				else fprintf(mout, "%d", phys[j]);
			}
		}
		fprintf(mout, "|\n");

		for (int pid = 0; pid < procCnt; ++pid) {
			// 종료되지 않은 프로세스나 이번 cycle에 종료된 프로세스만 출력
			if (pid != term && proc[pid].line == proc[pid].code.size()) continue;
			vector<Entry>& table = proc[pid].table;

			fprintf(mout, ">> pid(%d) %-20s", pid, "Page Table(AID) : ");
			for (int i = 0; i < table.size(); i += 4) {
				fprintf(mout, "|");
				for (int j = i; j < i+4; ++j) {
					int aid = table[j].aid;
					if (aid == 0) fprintf(mout, "-");
					else fprintf(mout, "%d", aid);
				}
			}
			fprintf(mout, "|\n");

			fprintf(mout, ">> pid(%d) %-20s", pid, "Page Table(Valid) : ");
			for (int i = 0; i < table.size(); i += 4) {
				fprintf(mout, "|");
				for (int j = i; j < i+4; ++j) {
					if (table[j].aid == 0) fprintf(mout, "-");
					else if (table[j].valid) fprintf(mout, "1");
					else fprintf(mout, "0");
				}
			}
			fprintf(mout, "|\n");
		}
		fprintf(mout, "\n");
		// 메모리상에서 종료된 프로세스의 정보를 없앰
		if (term != -1) terminateProc(term);
	}
	fprintf(mout, "page fault = %d\n", pagefault);
	// 파일 출력 종료
	fclose(sout);
	fclose(mout);
	// page fault rate 임시 출력
	// cout << (double)pagefault / accessCnt << "\n";
}
