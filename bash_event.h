/* bash_event.h -- termx 源码级 shell 事件通道接口。 */

/* Copyright (C) 2025 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

#if !defined (_BASH_EVENT_H_)
#define _BASH_EVENT_H_

/* Hook 只在上游文件的少量边界处调用；实现在 bash_event.c 中。
   非 Linux 平台没有通道实现，函数体退化为空操作（首批只构建 Linux）。 */

/* A：即将生成/打印主提示符。 */
extern void bash_event_prompt_start (void);
/* B：提示符已就绪，马上读取输入。 */
extern void bash_event_prompt_end (void);
/* C：即将执行一条已解析的命令；text 为 NULL 时不发事件。 */
extern void bash_event_command_start (char *text);
/* D：命令执行结束，code 为 shell 的权威退出状态。 */
extern void bash_event_command_end (int code);
/* X：输入未执行即被放弃（解析失败等）。 */
extern void bash_event_command_abort (void);
/* 顶层跳转：活动命令发 D，否则 DISCARD 发 X。 */
extern void bash_event_reader_jump (int code, int exit_value);
/* 退出前关闭通道（不写日志、不输出 token）。 */
extern void bash_event_shutdown (void);

#endif /* _BASH_EVENT_H_ */
