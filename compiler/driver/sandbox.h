// Песочница worker-а онлайн-сервиса (Р49; docs/service/README.md,
// «Изоляция»): процесс запирается до чтения входа.
#pragma once

#include <string>

namespace dp {

// Запирает процесс необратимо: PR_SET_NO_NEW_PRIVS, PR_SET_DUMPABLE = 0
// и фильтр seccomp на все потоки (TSYNC). Дальше разрешены только read,
// write, память без PROT_EXEC, futex и выход; любой другой
// системный вызов убивает процесс (SIGSYS). Потоки создаются до вызова.
// false — запереть не удалось: error — почему; работать дальше нельзя.
bool lock_down(std::string& error);

}  // namespace dp
