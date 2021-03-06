/* 
 * Copyright (c) [2010-2019] zhenfei.mzf@gmail.com rights reserved.
 * 
 * AntiSpy is licensed under the Mulan PSL v1.
 * You can use this software according to the terms and conditions of the Mulan PSL v1.
 * You may obtain a copy of Mulan PSL v1 at:
 *
 *     http://license.coscl.org.cn/MulanPSL
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v1 for more details.
*/
#ifndef _LIST_HANDLES_H_
#define _LIST_HANDLES_H_

#include "../../Common/Common.h"
#include <vector>
#include <algorithm>

using namespace std;

class CListHandles
{
public:
	CListHandles();
	~CListHandles();
	BOOL GetHandles(ULONG nPid, ULONG pEprocess, vector<HANDLE_INFO> &vectorHandles);
	BOOL CloseHandle(ULONG nPid, ULONG pEprocess, BOOL bForce, ULONG hHandle, ULONG pObject);
};

#endif