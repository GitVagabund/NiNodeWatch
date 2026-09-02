# NiNodeWatch 0.1.1

Диагностический SKSE-плагин для конкретного CTD в Skyrim SE 1.5.97. Он следит за изменениями `NiNode::children` и записывает последние операции вместе со стеком вызовов, если массив детей выглядит повреждённым либо игра падает с access violation.

## Ограничения

- Только Skyrim SE **1.5.97.0**. На другой версии плагин откажется загружаться.
- Адрес `SkyrimSE.exe+0x1CD9B0` взят из приложенного дампа. Это функция компактизации массива, упавшая по `+0x1CDA0B`.
- Плагин диагностический: он не отменяет операцию и не подавляет CTD.
- Обработчик исключений реагирует только на access violation внутри исследуемой
  функции `SkyrimSE.exe+0x1CD9B0..0x1CDAAF`. Служебные исключения других
  SKSE-плагинов (в том числе FreezeLogger) игнорируются.
- Hook добавляет небольшой overhead к операциям изменения `NiNode`. После нахождения виновника его нужно удалить.

## Сборка

Нужно установить:

1. Visual Studio 2022 Community с workload **Desktop development with C++**.
2. Git for Windows.
3. CMake (обычно входит в workload Visual Studio).

Открой **Developer PowerShell for VS 2022** в папке проекта и выполни:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build.ps1
```

Скрипт сам скачает `vcpkg`, CommonLibSSE-NG и MinHook. Готовый файл появится здесь:

```text
dist\NiNodeWatch\SKSE\Plugins\NiNodeWatch.dll
```

## Установка в MO2

Создай пустой мод `NiNodeWatch`, затем положи DLL по структуре:

```text
NiNodeWatch\SKSE\Plugins\NiNodeWatch.dll
```

Включи мод и запусти игру через SKSE. В обычном SKSE-логе должна появиться строка `NiNodeWatch hooks installed`.

## Что прислать после следующего CTD

В папке:

```text
Documents\My Games\Skyrim Special Edition\SKSE
```

появится файл вида:

```text
NiNodeWatch-incident-20260901-231122-T11556-1.log
```

Пришли его вместе с новым Crash Logger log. В incident-файле будут:

- `NiNode*`, имя узла и адрес массива `children`;
- `data / capacity / freeIdx / size / growBy`;
- последние `AttachChild`, `DetachChild*`, `SetAt*` и `Compact`;
- стек каждой операции в виде `hdtsmp64.dll+RVA`, `SkyrimSE.exe+RVA` и т. п.;
- отметка `SAME_NODE` для событий, относившихся к узлу, который оказался повреждён.

Если CTD не случится, но плагин заранее увидит невалидный `children._data`, он всё равно создаст incident-файл и затем передаст управление игре без изменений.

## Удаление

После диагностики выключи мод `NiNodeWatch`. ESP/ESL нет, сохранения он не изменяет.
