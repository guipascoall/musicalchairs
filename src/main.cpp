#include <iostream>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>

constexpr int NUM_JOGADORES = 4;

class counting_semaphore {
    std::mutex mtx;
    std::condition_variable cv;
    int count;
public:
    counting_semaphore(int n) : count(n) {}

    void acquire() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]{ return count > 0; });
        count--;
    }

    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mtx);
        if(count > 0) {
            count--;
            return true;
        }
        return false;
    }

    void release(int n = 1) {
        std::lock_guard<std::mutex> lock(mtx);
        count += n;
        cv.notify_all();
    }
};

counting_semaphore cadeira_sem(NUM_JOGADORES - 1);
std::mutex music_mutex;
std::condition_variable music_cv;
std::atomic<bool> musica_parada{false};
std::mutex cout_mutex;
std::atomic<int> jogadores_sentados{0};
std::atomic<bool> jogo_terminou{false};

class JogoDasCadeiras {
    std::vector<bool> jogadores_ativos;
    int cadeiras;
    int rodada = 1;
    std::mutex jogo_mutex;
    std::condition_variable rodada_cv;
    bool rodada_em_andamento{false};

public:
    JogoDasCadeiras() : jogadores_ativos(NUM_JOGADORES, true), cadeiras(NUM_JOGADORES - 1) {}

    void iniciar_rodada() {
        std::lock_guard<std::mutex> lock(jogo_mutex);
        rodada_em_andamento = true;
        std::lock_guard<std::mutex> cout_lock(cout_mutex);
        std::cout << "\n--- Rodada " << rodada++ << " ---\n";
        std::cout << "Jogadores restantes: " << get_jogadores_ativos() 
                  << " | Cadeiras: " << cadeiras << "\n";
        std::cout << "A música está tocando... 🎵\n";
    }

    void parar_musica() {
        musica_parada = true;
        music_cv.notify_all();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "\n> A música parou! Os jogadores estão tentando se sentar...\n";
        }
    }

    bool tentar_sentar(int id) {
        std::unique_lock<std::mutex> lock(jogo_mutex);
        if (jogadores_sentados >= cadeiras) {
            return false;
        }
        
        if (cadeira_sem.try_acquire()) {
            jogadores_sentados++;
            std::lock_guard<std::mutex> cout_lock(cout_mutex);
            std::cout << "[Cadeira] Ocupada por P" << id+1 << "\n";
            return true;
        }
        return false;
    }

    void finalizar_rodada() {
        std::unique_lock<std::mutex> lock(jogo_mutex);
        rodada_em_andamento = false;
        
        // Verifica se precisamos eliminar alguém
        int ativos = get_jogadores_ativos();
        if (ativos > 1 && jogadores_sentados < ativos - 1) {
            for (int i = 0; i < NUM_JOGADORES; ++i) {
                if (jogadores_ativos[i] && !cadeira_sem.try_acquire()) {
                    jogadores_ativos[i] = false;
                    std::lock_guard<std::mutex> cout_lock(cout_mutex);
                    std::cout << "Jogador P" << i+1 << " foi eliminado!\n";
                    break; // Elimina apenas um por rodada
                }
            }
        }
        
        rodada_cv.notify_all();
    }

    void esperar_proxima_rodada(int id) {
        std::unique_lock<std::mutex> lock(jogo_mutex);
        rodada_cv.wait(lock, [this, id] { 
            return !rodada_em_andamento || jogo_terminou || !jogadores_ativos[id]; 
        });
    }

    bool jogador_ativo(int id) const { return jogadores_ativos[id]; }
    int get_jogadores_ativos() const { 
        return std::count(jogadores_ativos.begin(), jogadores_ativos.end(), true); 
    }
    void reduzir_cadeiras() { 
        if (cadeiras > 0) cadeiras--; 
        jogadores_sentados = 0;
        cadeira_sem.release(NUM_JOGADORES); // Reset do semáforo
    }
    bool tem_vencedor() const { return get_jogadores_ativos() == 1; }
    int get_vencedor() const {
        for (int i = 0; i < NUM_JOGADORES; ++i)
            if (jogadores_ativos[i]) return i+1;
        return -1;
    }
};

void jogador_thread(int id, JogoDasCadeiras& jogo) {
    while (!jogo_terminou && jogo.jogador_ativo(id)) {
        // Espera a música parar
        {
            std::unique_lock<std::mutex> lock(music_mutex);
            music_cv.wait(lock, [&]{ return musica_parada.load() || jogo_terminou; });
        }
        
        if (jogo_terminou || !jogo.jogador_ativo(id)) break;
        
        // Tenta sentar
        if (jogo.tentar_sentar(id)) {
            jogo.esperar_proxima_rodada(id);
        } else {
            break;
        }
    }
}

void coordenador_thread(JogoDasCadeiras& jogo) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(1, 3);
    
    while (!jogo.tem_vencedor() && !jogo_terminou) {
        jogo.iniciar_rodada();
        std::this_thread::sleep_for(std::chrono::seconds(dist(gen)));
        
        jogo.parar_musica();
        
        // Espera todos tentarem sentar
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        jogo.finalizar_rodada();
        
        if (!jogo.tem_vencedor()) {
            jogo.reduzir_cadeiras();
            musica_parada = false;
        }
    }
    
    jogo_terminou = true;
    music_cv.notify_all();
    
    if (jogo.tem_vencedor()) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\n🏆 Vencedor: Jogador P" << jogo.get_vencedor() << "! Parabéns! 🏆\n";
    }
}

int main() {
    std::cout << "-----------------------------------------------\n";
    std::cout << "Bem-vindo ao Jogo das Cadeiras Concorrente!\n";
    std::cout << "-----------------------------------------------\n";

    JogoDasCadeiras jogo;
    std::vector<std::thread> jogadores;
    
    for (int i = 0; i < NUM_JOGADORES; ++i) {
        jogadores.emplace_back(jogador_thread, i, std::ref(jogo));
    }
    
    std::thread coordenador(coordenador_thread, std::ref(jogo));
    
    for (auto& t : jogadores) t.join();
    coordenador.join();
    
    std::cout << "-----------------------------------------------\n";
    return 0;
}