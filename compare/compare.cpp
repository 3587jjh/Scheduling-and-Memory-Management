#include <iostream>
#include <fstream>
#include <string>
#include <vector>
using namespace std;

int main(int argc, char* argv[]) {
	ifstream lFile(argv[1]), rFile(argv[2]);
	vector<string> lv, rv;
	string buff;

	while (lFile.peek() != EOF) {
		getline(lFile, buff);
		lv.push_back(buff);
	}
	while (rFile.peek() != EOF) {
		getline(rFile, buff);
		rv.push_back(buff);
	}
	lFile.close();
	rFile.close();

	while (!lv.empty() && lv.back().empty())
		lv.pop_back();
	while (!rv.empty() && rv.back().empty())
		rv.pop_back();

	if (lv.size() != rv.size()) {
		cout << "다릅니다\n";
		return 0;
	}
	int sz = lv.size();
	for (int i = 0; i < sz; ++i) {
		string A = lv[i], B = rv[i];
		if (A.size() > B.size()) swap(A, B);
		for (int j = 0; j < A.size(); ++j)
			if (A[j] != B[j]) {
				cout << "다릅니다\n";
				return 0;
			}
	}
	cout << "같습니다\n";
}
