#pragma once
#include <array>
#include <vector>
#include <list>
#include <queue>
#include <stack>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "Types.h"
using namespace std;

template<typename Type, uint32 Size>
using Array = array<Type, Size>;

template<typename Type>
using Vector = vector<Type>;          // ǥ�� allocator ���

template<typename Type>
using List = list<Type>;

template<typename Key, typename Type, typename Pred = less<Key>>
using Map = map<Key, Type, Pred>;

template<typename Key, typename Pred = less<Key>>
using Set = set<Key, Pred>;

template<typename Type>
using Deque = deque<Type>;

template<typename Type, typename Container = Deque<Type>>
using Queue = queue<Type, Container>;

template<typename Type, typename Container = Deque<Type>>
using Stack = stack<Type, Container>;

template<typename Type, typename Container = Vector<Type>, typename Pred = less<typename Container::value_type>>
using PriorityQueue = priority_queue<Type, Container, Pred>;

using String = string;                // char 기반 UTF-8 (크로스 플랫폼)

template<typename Key, typename Type, typename Hasher = hash<Key>, typename KeyEq = equal_to<Key>>
using HashMap = unordered_map<Key, Type, Hasher, KeyEq>;

template<typename Key, typename Hasher = hash<Key>, typename KeyEq = equal_to<Key>>
using HashSet = unordered_set<Key, Hasher, KeyEq>;