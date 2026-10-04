// Logger.h — journal de bord asynchrone.
//
// Règle de conception : AUCUN appel bloquant dans la boucle de rendu du jeu.
//  * Log() formate le message puis le dépose dans une file en mémoire
//    (section critique de quelques microsecondes, pas d'écriture disque).
//  * Un thread dédié vide la file et écrit dans le fichier.
//  * Si la file est pleine, les plus anciens messages sont perdus (compteur
//    écrit dans le journal) plutôt que de bloquer le jeu.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "LogLevel.h"

namespace hands::log {

class Logger {
public:
    Logger();
    ~Logger();  // arrête et joint le thread (utilisé par les tests uniquement)
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Journal global du processus. NOTE C++ : l'objet est alloué avec `new` et
    // JAMAIS détruit, volontairement. Une DLL est déchargée pendant l'arrêt du
    // processus, sous le « loader lock » Windows : joindre un thread à ce moment
    // provoque un blocage (deadlock). Laisser fuir l'objet est la solution
    // standard et sans danger (le système libère tout à la fin du processus).
    static Logger& Get();

    // Ouvre le fichier (le précédent est renommé en *.prev.log). Retourne false
    // si le fichier est inaccessible ; les messages restent alors en mémoire
    // (bornés) et rien ne plante.
    bool Open(const std::filesystem::path& file);

    void SetLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel Level() const { return level_.load(std::memory_order_relaxed); }
    bool IsEnabled(LogLevel level) const { return level <= Level(); }

    // Écriture « printf » : Log(LogLevel::Info, "x=%d", 3).
    // NOTE C++ : `...` = nombre variable d'arguments (comme `params` en C#),
    // mais NON typé : le compilateur vérifie le format grâce à l'attribut GCC.
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    void Log(LogLevel level, const char* fmt, ...);
    void LogV(LogLevel level, const char* fmt, va_list args);

    // Attend (au plus `timeout`) que tout soit écrit sur le disque.
    // À appeler hors boucle de rendu (ex. destruction de l'instance OpenXR).
    bool Flush(std::chrono::milliseconds timeout);

    std::filesystem::path Path() const;

private:
    void Enqueue(std::string line);
    void WorkerLoop();

    static constexpr size_t kMaxQueued = 8192;

    std::atomic<LogLevel> level_{LogLevel::Info};

    mutable std::mutex mutex_;          // protège tout ce qui suit
    std::condition_variable wake_;      // réveille le thread d'écriture
    std::condition_variable drained_;   // signale « file vidée et écrite »
    std::deque<std::string> queue_;
    size_t dropped_ = 0;
    bool writing_ = false;
    bool stop_ = false;
    bool workerStarted_ = false;
    bool fileOpen_ = false;
    std::filesystem::path path_;
    std::thread worker_;
};

}  // namespace hands::log
