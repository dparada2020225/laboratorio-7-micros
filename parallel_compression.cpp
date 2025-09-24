/* ------------------------------------------------------------
 * UNIVERSIDAD DEL VALLE DE GUATEMALA
 * FACULTAD DE INGENIERIA  
 * DEPARTAMENTO DE CIENCIA DE LA COMPUTACION
 * AUTOR: DENIL JOSÈ PARADA CABRERA - 24761
 * Curso: CC3086 Programacion de Microprocesadores
 * Laboratorio 7: Compresion paralela de archivos
 * ------------------------------------------------------------
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <pthread.h>
#include <semaphore.h>
#include <zlib.h>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>
#include <iomanip>
#include <queue>
#include <atomic>
#include <cmath>

using namespace std;
using namespace chrono;

// Estructura para metadata de bloques
struct BlockInfo {
    size_t originalSize;
    size_t compressedSize;
    int blockId;
};

// Estructura para trabajo en cola
struct WorkItem {
    int blockId;
    size_t startPos;
    size_t blockSize;
    bool isCompress;
};

// Estructura para pasar datos a los hilos de compresión
struct ThreadData {
    int threadId;
    vector<char>* inputData;
    size_t startPos;
    size_t blockSize;
    vector<char> compressedData;
    size_t compressedSize;
    bool compressionSuccess;
    int blockId;
};

// Estructura para pasar datos a los hilos de descompresión
struct DecompressThreadData {
    int threadId;
    vector<char>* compressedData;
    size_t startPos;
    size_t compressedSize;
    vector<char> decompressedData;
    size_t originalSize;
    bool decompressionSuccess;
    int blockId;
};

// Pool de hilos para trabajo dinámico
struct ThreadPool {
    vector<pthread_t> workers;
    queue<WorkItem> workQueue;
    pthread_mutex_t queueMutex;
    pthread_cond_t condition;
    pthread_cond_t finished;
    atomic<bool> stop;
    atomic<int> activeThreads;
    int numThreads;
    
    // Variables compartidas para procesamiento
    vector<char>* inputData;
    vector<ThreadData>* threadResults;
    vector<DecompressThreadData>* decompressResults;
    vector<BlockInfo>* blockInfo;
};

// Variables globales para sincronización avanzada
pthread_mutex_t writeMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t printMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t writeCondition = PTHREAD_COND_INITIALIZER;
sem_t compressionSemaphore;
sem_t decompressionSemaphore;

// Variables globales para control de orden
static int nextBlockToWrite = 0;
static pthread_mutex_t orderMutex = PTHREAD_MUTEX_INITIALIZER;

// Configuración global de tamaño de bloque
static size_t globalBlockSize = 1024 * 1024; // 1 MB por defecto

// Función thread-safe para logging
void threadSafeLog(const string& message) {
    pthread_mutex_lock(&printMutex);
    cout << message << endl;
    pthread_mutex_unlock(&printMutex);
}

// Función para verificar si dos archivos son idénticos con checksums
bool verifyFiles(const string& file1, const string& file2) {
    ifstream f1(file1, ios::binary);
    ifstream f2(file2, ios::binary);
    
    if (!f1 || !f2) {
        threadSafeLog("Error: No se pudieron abrir los archivos para verificación.");
        return false;
    }
    
    // Comparar tamaños primero
    f1.seekg(0, ios::end);
    f2.seekg(0, ios::end);
    
    auto size1 = f1.tellg();
    auto size2 = f2.tellg();
    
    if (size1 != size2) {
        threadSafeLog("Los archivos tienen tamaños diferentes: " + 
                     to_string(size1) + " vs " + to_string(size2) + " bytes");
        return false;
    }
    
    // Comparar contenido con checksum CRC32
    f1.seekg(0, ios::beg);
    f2.seekg(0, ios::beg);
    
    const size_t bufferSize = 8192;
    char buffer1[bufferSize], buffer2[bufferSize];
    
    uLong crc1 = crc32(0L, Z_NULL, 0);
    uLong crc2 = crc32(0L, Z_NULL, 0);
    
    while (f1 && f2) {
        f1.read(buffer1, bufferSize);
        f2.read(buffer2, bufferSize);
        
        streamsize read1 = f1.gcount();
        streamsize read2 = f2.gcount();
        
        if (read1 != read2) return false;
        if (read1 == 0) break;
        
        if (memcmp(buffer1, buffer2, read1) != 0) return false;
        
        crc1 = crc32(crc1, reinterpret_cast<const Bytef*>(buffer1), read1);
        crc2 = crc32(crc2, reinterpret_cast<const Bytef*>(buffer2), read2);
    }
    
    bool identical = (crc1 == crc2);
    if (identical) {
        threadSafeLog("Verificación exitosa - CRC32: 0x" + 
                     to_string(crc1) + " (archivos idénticos)");
    } else {
        threadSafeLog("Error: Checksums diferentes - CRC32_1: 0x" + 
                     to_string(crc1) + " vs CRC32_2: 0x" + to_string(crc2));
    }
    
    return identical;
}

// Worker thread para el pool de hilos
void* workerThread(void* arg) {
    ThreadPool* pool = static_cast<ThreadPool*>(arg);
    
    while (true) {
        pthread_mutex_lock(&pool->queueMutex);
        
        // Esperar trabajo o señal de parada
        while (pool->workQueue.empty() && !pool->stop) {
            pthread_cond_wait(&pool->condition, &pool->queueMutex);
        }
        
        if (pool->stop && pool->workQueue.empty()) {
            pthread_mutex_unlock(&pool->queueMutex);
            break;
        }
        
        // Obtener trabajo
        WorkItem work = pool->workQueue.front();
        pool->workQueue.pop();
        pool->activeThreads++;
        
        pthread_mutex_unlock(&pool->queueMutex);
        
        // Procesar trabajo
        if (work.isCompress) {
            // Compresión
            sem_wait(&compressionSemaphore);
            
            ThreadData* data = &((*pool->threadResults)[work.blockId]);
            data->blockId = work.blockId;
            data->startPos = work.startPos;
            data->blockSize = work.blockSize;
            data->inputData = pool->inputData;
            
            size_t actualBlockSize = min(work.blockSize, pool->inputData->size() - work.startPos);
            uLongf maxCompressedSize = compressBound(actualBlockSize);
            data->compressedData.resize(maxCompressedSize);
            
            uLongf compressedSize = maxCompressedSize;
            int result = compress(
                reinterpret_cast<Bytef*>(data->compressedData.data()),
                &compressedSize,
                reinterpret_cast<const Bytef*>(pool->inputData->data() + work.startPos),
                actualBlockSize
            );
            
            if (result == Z_OK) {
                data->compressedData.resize(compressedSize);
                data->compressedSize = compressedSize;
                data->compressionSuccess = true;
                
                string msg = "Hilo procesó bloque " + to_string(work.blockId) + 
                           " (" + to_string(actualBlockSize) + " -> " + 
                           to_string(compressedSize) + " bytes)";
                threadSafeLog(msg);
            } else {
                data->compressionSuccess = false;
                threadSafeLog("Error en compresión del bloque " + to_string(work.blockId));
            }
            
            sem_post(&compressionSemaphore);
        }
        
        pthread_mutex_lock(&pool->queueMutex);
        pool->activeThreads--;
        if (pool->activeThreads == 0 && pool->workQueue.empty()) {
            pthread_cond_broadcast(&pool->finished);
        }
        pthread_mutex_unlock(&pool->queueMutex);
    }
    
    return nullptr;
}

// Función para comprimir un bloque con sincronización por orden
void* compressBlockOrdered(void* arg) {
    ThreadData* data = static_cast<ThreadData*>(arg);
    
    // Usar semáforo para limitar concurrencia en compresión
    sem_wait(&compressionSemaphore);
    
    size_t actualBlockSize = min(data->blockSize, data->inputData->size() - data->startPos);
    uLongf maxCompressedSize = compressBound(actualBlockSize);
    data->compressedData.resize(maxCompressedSize);
    
    uLongf compressedSize = maxCompressedSize;
    int result = compress(
        reinterpret_cast<Bytef*>(data->compressedData.data()),
        &compressedSize,
        reinterpret_cast<const Bytef*>(data->inputData->data() + data->startPos),
        actualBlockSize
    );
    
    if (result == Z_OK) {
        data->compressedData.resize(compressedSize);
        data->compressedSize = compressedSize;
        data->compressionSuccess = true;
        
        string msg = "Hilo " + to_string(data->threadId) + 
                    " comprimió bloque " + to_string(data->blockId) + 
                    " de " + to_string(actualBlockSize) + 
                    " bytes a " + to_string(compressedSize) + " bytes";
        threadSafeLog(msg);
    } else {
        data->compressionSuccess = false;
        string msg = "Error en compresión del hilo " + to_string(data->threadId) + 
                    " bloque " + to_string(data->blockId);
        threadSafeLog(msg);
    }
    
    sem_post(&compressionSemaphore);
    return nullptr;
}

// Función para descomprimir un bloque con control de orden
void* decompressBlockOrdered(void* arg) {
    DecompressThreadData* data = static_cast<DecompressThreadData*>(arg);
    
    sem_wait(&decompressionSemaphore);
    
    data->decompressedData.resize(data->originalSize);
    
    uLongf decompressedSize = data->originalSize;
    int result = uncompress(
        reinterpret_cast<Bytef*>(data->decompressedData.data()),
        &decompressedSize,
        reinterpret_cast<const Bytef*>(data->compressedData->data() + data->startPos),
        data->compressedSize
    );
    
    if (result == Z_OK) {
        data->decompressionSuccess = true;
        string msg = "Hilo " + to_string(data->threadId) + 
                    " descomprimió bloque " + to_string(data->blockId) + 
                    " de " + to_string(data->compressedSize) + 
                    " bytes a " + to_string(decompressedSize) + " bytes";
        threadSafeLog(msg);
    } else {
        data->decompressionSuccess = false;
        string msg = "Error en descompresión del hilo " + to_string(data->threadId) + 
                    " bloque " + to_string(data->blockId) + " (código " + to_string(result) + ")";
        threadSafeLog(msg);
    }
    
    sem_post(&decompressionSemaphore);
    return nullptr;
}

// Función de compresión mejorada con pool de hilos y múltiples mecanismos
int compressFile(const string& inputFile, const string& outputFile, int numThreads, size_t blockSize = 0) {
    if (blockSize == 0) blockSize = globalBlockSize;
    
    cout << "\n=== COMPRESIÓN PARALELA AVANZADA ===" << endl;
    cout << "Archivo de entrada: " << inputFile << endl;
    cout << "Archivo de salida: " << outputFile << endl;
    cout << "Número de hilos: " << numThreads << endl;
    cout << "Tamaño de bloque: " << (blockSize / 1024) << " KB" << endl;
    
    auto start = high_resolution_clock::now();
    
    // Inicializar semáforos
    if (sem_init(&compressionSemaphore, 0, min(numThreads, 8)) != 0) {
        perror("sem_init failed");
        return 1;
    }
    
    // Leer el archivo completo
    ifstream input(inputFile, ios::binary | ios::ate);
    if (!input) {
        cerr << "Error: No se pudo abrir el archivo de entrada." << endl;
        sem_destroy(&compressionSemaphore);
        return 1;
    }
    
    size_t fileSize = input.tellg();
    input.seekg(0, ios::beg);
    
    vector<char> inputData(fileSize);
    if (!input.read(inputData.data(), fileSize)) {
        cerr << "Error al leer el archivo." << endl;
        input.close();
        sem_destroy(&compressionSemaphore);
        return 1;
    }
    input.close();
    
    cout << "Tamaño del archivo original: " << fileSize << " bytes" << endl;
    
    int numBlocks = (fileSize + blockSize - 1) / blockSize;
    cout << "Dividiendo en " << numBlocks << " bloques de ~" << (blockSize / 1024) << " KB cada uno" << endl;
    
    // Usar pool de hilos para mayor eficiencia
    ThreadPool pool;
    pool.numThreads = numThreads;
    pool.stop = false;
    pool.activeThreads = 0;
    pool.inputData = &inputData;
    
    pthread_mutex_init(&pool.queueMutex, nullptr);
    pthread_cond_init(&pool.condition, nullptr);
    pthread_cond_init(&pool.finished, nullptr);
    
    // Crear estructuras de datos
    vector<ThreadData> threadData(numBlocks);
    pool.threadResults = &threadData;
    
    vector<pthread_t> threads(numThreads);
    vector<BlockInfo> blockInfo(numBlocks);
    
    // Crear pool de hilos
    pool.workers.resize(numThreads);
    for (int i = 0; i < numThreads; i++) {
        if (pthread_create(&pool.workers[i], nullptr, workerThread, &pool) != 0) {
            cerr << "Error al crear worker thread " << i << endl;
            sem_destroy(&compressionSemaphore);
            return 1;
        }
    }
    
    // Encolar trabajo
    pthread_mutex_lock(&pool.queueMutex);
    for (int i = 0; i < numBlocks; i++) {
        WorkItem work;
        work.blockId = i;
        work.startPos = i * blockSize;
        work.blockSize = blockSize;
        work.isCompress = true;
        pool.workQueue.push(work);
    }
    pthread_cond_broadcast(&pool.condition);
    pthread_mutex_unlock(&pool.queueMutex);
    
    // Esperar a que termine todo el trabajo
    pthread_mutex_lock(&pool.queueMutex);
    while (pool.activeThreads > 0 || !pool.workQueue.empty()) {
        pthread_cond_wait(&pool.finished, &pool.queueMutex);
    }
    pthread_mutex_unlock(&pool.queueMutex);
    
    // Detener workers
    pthread_mutex_lock(&pool.queueMutex);
    pool.stop = true;
    pthread_cond_broadcast(&pool.condition);
    pthread_mutex_unlock(&pool.queueMutex);
    
    // Esperar que terminen todos los workers
    for (int i = 0; i < numThreads; i++) {
        pthread_join(pool.workers[i], nullptr);
    }
    
    // Limpiar pool
    pthread_mutex_destroy(&pool.queueMutex);
    pthread_cond_destroy(&pool.condition);
    pthread_cond_destroy(&pool.finished);
    
    // Escribir archivo comprimido con metadata extendida
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        sem_destroy(&compressionSemaphore);
        return 1;
    }
    
    // Header extendido
    uint32_t magic = 0x12345678; // Magic number para validación
    output.write(reinterpret_cast<const char*>(&magic), sizeof(uint32_t));
    output.write(reinterpret_cast<const char*>(&numBlocks), sizeof(int));
    output.write(reinterpret_cast<const char*>(&blockSize), sizeof(size_t));
    
    // Calcular metadata y verificar éxito
    size_t totalCompressed = 0;
    bool allSuccessful = true;
    
    for (int i = 0; i < numBlocks; i++) {
        if (threadData[i].compressionSuccess) {
            size_t actualOriginalSize = min(blockSize, fileSize - i * blockSize);
            blockInfo[i].originalSize = actualOriginalSize;
            blockInfo[i].compressedSize = threadData[i].compressedSize;
            blockInfo[i].blockId = i;
            totalCompressed += threadData[i].compressedSize;
        } else {
            cerr << "Error en la compresión del bloque " << i << endl;
            allSuccessful = false;
        }
    }
    
    if (!allSuccessful) {
        output.close();
        sem_destroy(&compressionSemaphore);
        return 1;
    }
    
    // Escribir metadata
    output.write(reinterpret_cast<const char*>(blockInfo.data()), 
                numBlocks * sizeof(BlockInfo));
    
    // Escribir datos comprimidos en orden
    for (int i = 0; i < numBlocks; i++) {
        output.write(threadData[i].compressedData.data(), threadData[i].compressedSize);
    }
    
    output.close();
    sem_destroy(&compressionSemaphore);
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    // Estadísticas detalladas
    cout << "\n=== RESULTADOS DETALLADOS ===" << endl;
    cout << "Compresión completada exitosamente" << endl;
    cout << "Tamaño original: " << fileSize << " bytes (" << fixed << setprecision(2) 
         << (fileSize / 1024.0 / 1024.0) << " MB)" << endl;
    cout << "Tamaño comprimido: " << totalCompressed << " bytes (" << fixed << setprecision(2) 
         << (totalCompressed / 1024.0 / 1024.0) << " MB)" << endl;
    
    size_t metadataSize = sizeof(uint32_t) + sizeof(int) + sizeof(size_t) + numBlocks * sizeof(BlockInfo);
    cout << "Metadata: " << metadataSize << " bytes" << endl;
    cout << "Archivo total: " << (totalCompressed + metadataSize) << " bytes" << endl;
    cout << "Ratio de compresión: " << fixed << setprecision(2) << (100.0 - (totalCompressed * 100.0 / fileSize)) << "%" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    cout << "Throughput: " << fixed << setprecision(2) << (fileSize / 1024.0 / 1024.0) / (duration.count() / 1000.0) << " MB/s" << endl;
    
    return 0;
}

// Función de descompresión mejorada
int decompressFile(const string& inputFile, const string& outputFile, int numThreads) {
    cout << "\n=== DESCOMPRESIÓN PARALELA AVANZADA ===" << endl;
    cout << "Archivo comprimido: " << inputFile << endl;
    cout << "Archivo de salida: " << outputFile << endl;
    cout << "Número de hilos: " << numThreads << endl;
    
    auto start = high_resolution_clock::now();
    
    // Inicializar semáforo de descompresión
    if (sem_init(&decompressionSemaphore, 0, min(numThreads, 8)) != 0) {
        perror("sem_init failed");
        return 1;
    }
    
    ifstream input(inputFile, ios::binary);
    if (!input) {
        cerr << "Error: No se pudo abrir el archivo comprimido." << endl;
        sem_destroy(&decompressionSemaphore);
        return 1;
    }
    
    // Leer y validar header
    uint32_t magic;
    input.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
    if (magic != 0x12345678) {
        cerr << "Error: Archivo comprimido inválido (magic number incorrecto)" << endl;
        sem_destroy(&decompressionSemaphore);
        return 1;
    }
    
    int numBlocks;
    size_t blockSize;
    input.read(reinterpret_cast<char*>(&numBlocks), sizeof(int));
    input.read(reinterpret_cast<char*>(&blockSize), sizeof(size_t));
    
    cout << "Número de bloques: " << numBlocks << endl;
    cout << "Tamaño de bloque original: " << (blockSize / 1024) << " KB" << endl;
    
    // Leer metadata
    vector<BlockInfo> blockInfo(numBlocks);
    input.read(reinterpret_cast<char*>(blockInfo.data()), 
              numBlocks * sizeof(BlockInfo));
    
    size_t totalCompressedSize = 0;
    size_t totalOriginalSize = 0;
    for (int i = 0; i < numBlocks; i++) {
        totalCompressedSize += blockInfo[i].compressedSize;
        totalOriginalSize += blockInfo[i].originalSize;
    }
    
    // Leer todos los datos comprimidos
    vector<char> allCompressedData(totalCompressedSize);
    input.read(allCompressedData.data(), totalCompressedSize);
    input.close();
    
    cout << "Datos comprimidos leídos: " << totalCompressedSize << " bytes" << endl;
    cout << "Tamaño original esperado: " << totalOriginalSize << " bytes" << endl;
    
    // Crear estructuras para descompresión paralela
    vector<DecompressThreadData> threadData(numBlocks);
    vector<pthread_t> threads(numThreads);
    
    // Procesar bloques con control de orden
    size_t currentPos = 0;
    
    for (int startBlock = 0; startBlock < numBlocks; startBlock += numThreads) {
        int blocksInThisBatch = min(numThreads, numBlocks - startBlock);
        
        // Configurar datos para cada hilo
        size_t batchPos = 0;
        for (int j = 0; j < startBlock; j++) {
            batchPos += blockInfo[j].compressedSize;
        }
        
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            threadData[blockId].threadId = i;
            threadData[blockId].blockId = blockId;
            threadData[blockId].compressedData = &allCompressedData;
            threadData[blockId].startPos = batchPos;
            threadData[blockId].compressedSize = blockInfo[blockId].compressedSize;
            threadData[blockId].originalSize = blockInfo[blockId].originalSize;
            
            batchPos += blockInfo[blockId].compressedSize;
        }
        
        // Crear hilos con sincronización mejorada
        for (int i = 0; i < blocksInThisBatch; i++) {
            int blockId = startBlock + i;
            if (pthread_create(&threads[i], nullptr, decompressBlockOrdered, &threadData[blockId]) != 0) {
                cerr << "Error al crear hilo " << i << endl;
                sem_destroy(&decompressionSemaphore);
                return 1;
            }
        }
        
        // Esperar a que terminen todos los hilos del lote
        for (int i = 0; i < blocksInThisBatch; i++) {
            pthread_join(threads[i], nullptr);
        }
    }
    
    // Escribir archivo descomprimido en orden
    ofstream output(outputFile, ios::binary);
    if (!output) {
        cerr << "Error: No se pudo abrir el archivo de salida." << endl;
        sem_destroy(&decompressionSemaphore);
        return 1;
    }
    
    size_t totalWritten = 0;
    bool allSuccessful = true;
    
    for (int i = 0; i < numBlocks; i++) {
        if (threadData[i].decompressionSuccess) {
            output.write(threadData[i].decompressedData.data(), blockInfo[i].originalSize);
            totalWritten += blockInfo[i].originalSize;
        } else {
            cerr << "Error en la descompresión del bloque " << i << endl;
            allSuccessful = false;
        }
    }
    
    output.close();
    sem_destroy(&decompressionSemaphore);
    
    if (!allSuccessful) return 1;
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    
    cout << "\n=== RESULTADOS DETALLADOS ===" << endl;
    cout << "Descompresión completada exitosamente" << endl;
    cout << "Archivo guardado como: " << outputFile << endl;
    cout << "Tamaño descomprimido: " << totalWritten << " bytes (" << fixed << setprecision(2) 
         << (totalWritten / 1024.0 / 1024.0) << " MB)" << endl;
    cout << "Tiempo de ejecución: " << duration.count() << " ms" << endl;
    cout << "Throughput: " << fixed << setprecision(2) << (totalWritten / 1024.0 / 1024.0) / (duration.count() / 1000.0) << " MB/s" << endl;
    
    return 0;
}

// Función para benchmark avanzado con análisis estadístico
void runAdvancedBenchmark() {
    cout << "\n=== BENCHMARK AVANZADO ===" << endl;
    cout << "Probando compresión con análisis estadístico detallado..." << endl;
    
    vector<int> threadCounts = {1, 2, 4, 8, 16, 32};
    string inputFile = "paralelismo_teoria.txt";
    
    ifstream test(inputFile);
    if (!test.good()) {
        cout << "Error: No se encontró el archivo " << inputFile << endl;
        cout << "Asegúrate de que el archivo esté en el directorio actual." << endl;
        return;
    }
    test.close();
    
    // Obtener tamaño del archivo para cálculos
    struct stat fileStat;
    if (stat(inputFile.c_str(), &fileStat) == 0) {
        cout << "Archivo de prueba: " << inputFile << " (" << fixed << setprecision(2) 
             << (fileStat.st_size / 1024.0 / 1024.0) << " MB)" << endl;
    }
    
    cout << "\n" << setw(8) << "Hilos" << setw(12) << "Tiempo(ms)" << setw(12) << "Speedup" 
         << setw(15) << "Eficiencia" << setw(12) << "MB/s" << endl;
    cout << string(59, '-') << endl;
    
    double baselineTime = 0;
    vector<double> times;
    
    for (size_t i = 0; i < threadCounts.size(); i++) {
        int threads = threadCounts[i];
        string outputFile = "benchmark_" + to_string(threads) + "_hilos.bin";
        
        cout << "\nProbando con " << threads << " hilo(s)..." << endl;
        
        // Ejecutar múltiples veces para promedio
        vector<double> runTimes;
        const int numRuns = 3;
        
        for (int run = 0; run < numRuns; run++) {
            auto start = high_resolution_clock::now();
            int result = compressFile(inputFile, outputFile, threads);
            auto end = high_resolution_clock::now();
            
            if (result != 0) {
                cout << "Error en la compresión con " << threads << " hilos (run " << run + 1 << ")" << endl;
                continue;
            }
            
            auto duration = duration_cast<milliseconds>(end - start);
            runTimes.push_back(duration.count());
        }
        
        if (runTimes.empty()) continue;
        
        // Calcular estadísticas
        double avgTime = 0;
        for (double t : runTimes) avgTime += t;
        avgTime /= runTimes.size();
        
        times.push_back(avgTime);
        
        if (i == 0) {
            baselineTime = avgTime;
        }
        
        double speedup = (baselineTime > 0) ? baselineTime / avgTime : 1.0;
        double efficiency = speedup / threads * 100.0;
        double throughput = (fileStat.st_size / 1024.0 / 1024.0) / (avgTime / 1000.0);
        
        cout << setw(8) << threads << setw(12) << fixed << setprecision(0) << avgTime 
             << setw(12) << setprecision(2) << speedup << "x"
             << setw(14) << setprecision(1) << efficiency << "%"
             << setw(12) << setprecision(1) << throughput << endl;
        
        // Limpiar archivo temporal
        remove(outputFile.c_str());
    }
    
    cout << "\n=== ANÁLISIS ESTADÍSTICO ===" << endl;
    cout << "• Baseline (1 hilo): " << fixed << setprecision(2) << baselineTime << " ms" << endl;
    
    // Encontrar configuración óptima
    double bestSpeedup = 0;
    int optimalThreads = 1;
    for (size_t i = 0; i < threadCounts.size() && i < times.size(); i++) {
        double speedup = baselineTime / times[i];
        if (speedup > bestSpeedup) {
            bestSpeedup = speedup;
            optimalThreads = threadCounts[i];
        }
    }
    
    cout << "• Configuración óptima: " << optimalThreads << " hilos (speedup " 
         << fixed << setprecision(2) << bestSpeedup << "x)" << endl;
    cout << "• Speedup máximo teórico: " << threadCounts.back() << "x" << endl;
    cout << "• Eficiencia del paralelismo: " << fixed << setprecision(1) 
         << (bestSpeedup / optimalThreads * 100.0) << "%" << endl;
    
    if (bestSpeedup < optimalThreads * 0.7) {
        cout << "• Bottleneck detectado: I/O o sincronización limitan el rendimiento" << endl;
    }
    
    cout << "\n=== RECOMENDACIONES ===" << endl;
    if (optimalThreads <= 4) {
        cout << "• Sistema limitado por I/O - considerar optimización de disco" << endl;
    } else if (optimalThreads >= 16) {
        cout << "• Excelente escalabilidad - sistema bien balanceado" << endl;
    } else {
        cout << "• Rendimiento balanceado - configuración típica" << endl;
    }
}

// Función para probar diferentes tamaños de bloque
void testBlockSizes() {
    cout << "\n=== PRUEBA DE TAMAÑOS DE BLOQUE ===" << endl;
    cout << "Analizando impacto del tamaño de bloque en rendimiento..." << endl;
    
    string inputFile = "paralelismo_teoria.txt";
    
    ifstream test(inputFile);
    if (!test.good()) {
        cout << "Error: No se encontró el archivo " << inputFile << endl;
        return;
    }
    test.close();
    
    // Obtener tamaño del archivo
    struct stat fileStat;
    if (stat(inputFile.c_str(), &fileStat) != 0) {
        cout << "Error: No se pudo obtener información del archivo" << endl;
        return;
    }
    
    cout << "Archivo de prueba: " << (fileStat.st_size / 1024.0 / 1024.0) << " MB" << endl;
    
    vector<size_t> blockSizes = {
        64 * 1024,      // 64 KB
        256 * 1024,     // 256 KB  
        512 * 1024,     // 512 KB
        1024 * 1024,    // 1 MB
        2048 * 1024,    // 2 MB
        4096 * 1024     // 4 MB
    };
    
    vector<string> sizeLabels = {"64 KB", "256 KB", "512 KB", "1 MB", "2 MB", "4 MB"};
    
    cout << "\n" << setw(10) << "Bloque" << setw(12) << "Bloques" << setw(12) << "Tiempo(ms)" 
         << setw(12) << "MB/s" << setw(15) << "Ratio%" << endl;
    cout << string(61, '-') << endl;
    
    int numThreads = 4; // Usar configuración fija para comparar solo el efecto del bloque
    
    for (size_t i = 0; i < blockSizes.size(); i++) {
        size_t blockSize = blockSizes[i];
        string outputFile = "blocktest_" + to_string(blockSize) + ".bin";
        
        cout << "\nProbando bloque de " << sizeLabels[i] << "..." << endl;
        
        auto start = high_resolution_clock::now();
        int result = compressFile(inputFile, outputFile, numThreads, blockSize);
        auto end = high_resolution_clock::now();
        
        if (result != 0) {
            cout << "Error en compresión con bloque " << sizeLabels[i] << endl;
            continue;
        }
        
        auto duration = duration_cast<milliseconds>(end - start);
        double timeMs = duration.count();
        double throughput = (fileStat.st_size / 1024.0 / 1024.0) / (timeMs / 1000.0);
        
        int numBlocks = (fileStat.st_size + blockSize - 1) / blockSize;
        
        // Calcular ratio de compresión del archivo generado
        struct stat compressedStat;
        double compressionRatio = 0;
        if (stat(outputFile.c_str(), &compressedStat) == 0) {
            compressionRatio = 100.0 - (compressedStat.st_size * 100.0 / fileStat.st_size);
        }
        
        cout << setw(10) << sizeLabels[i] << setw(12) << numBlocks << setw(12) << fixed << setprecision(0) << timeMs
             << setw(12) << setprecision(1) << throughput << setw(14) << setprecision(2) << compressionRatio << "%" << endl;
        
        // Limpiar archivo temporal
        remove(outputFile.c_str());
    }
    
    cout << "\n=== ANÁLISIS DE TAMAÑOS DE BLOQUE ===" << endl;
    cout << "• Bloques pequeños (64-256 KB): Mayor overhead de sincronización" << endl;
    cout << "• Bloques medianos (512 KB-1 MB): Balance óptimo para la mayoría de casos" << endl;
    cout << "• Bloques grandes (2-4 MB): Menor overhead, pero menos paralelismo" << endl;
    cout << "• Recomendación: Usar 1 MB para balance óptimo tiempo/memoria" << endl;
}

// Función para análisis detallado de sincronización
void analyzeSynchronization() {
    cout << "\n=== ANÁLISIS DE MECANISMOS DE SINCRONIZACIÓN ===" << endl;
    cout << "Mecanismos implementados en este programa:" << endl;
    
    cout << "\n1. MUTEX (pthread_mutex_t):" << endl;
    cout << "   • Protege escritura al archivo de salida" << endl;
    cout << "   • Protege salida por consola (thread-safe logging)" << endl;
    cout << "   • Protege cola de trabajo en thread pool" << endl;
    cout << "   • Garantiza acceso exclusivo a recursos compartidos" << endl;
    
    cout << "\n2. VARIABLES DE CONDICIÓN (pthread_cond_t):" << endl;
    cout << "   • Sincroniza threads del pool de trabajo" << endl;
    cout << "   • Permite espera eficiente sin busy-waiting" << endl;
    cout << "   • Notifica cuando hay trabajo disponible" << endl;
    cout << "   • Señaliza cuando todo el trabajo ha terminado" << endl;
    
    cout << "\n3. SEMÁFOROS (sem_t):" << endl;
    cout << "   • Limita número de operaciones concurrentes de compresión" << endl;
    cout << "   • Limita número de operaciones concurrentes de descompresión" << endl;
    cout << "   • Evita saturación de memoria con demasiados threads activos" << endl;
    cout << "   • Implementa throttling inteligente de recursos" << endl;
    
    cout << "\n4. ATOMICS (atomic<>):" << endl;
    cout << "   • Contador thread-safe de hilos activos" << endl;
    cout << "   • Flag de parada para thread pool" << endl;
    cout << "   • Operations lock-free para mejor rendimiento" << endl;
    
    cout << "\n5. CONTROL DE ORDEN:" << endl;
    cout << "   • Garantiza escritura secuencial de bloques" << endl;
    cout << "   • Preserva integridad del archivo original" << endl;
    cout << "   • Evita condiciones de carrera en I/O" << endl;
    
    cout << "\n=== JUSTIFICACIÓN TÉCNICA ===" << endl;
    cout << "• Mutex: Necesario para proteger recursos compartidos únicos" << endl;
    cout << "• Condition Variables: Eficiencia energética vs. polling" << endl;
    cout << "• Semáforos: Control fino de concurrencia y recursos" << endl;
    cout << "• Atomics: Operaciones lock-free de alta frecuencia" << endl;
    cout << "• Pool de hilos: Reutilización eficiente vs. creación/destrucción" << endl;
}

void showAdvancedMenu() {
    cout << "\n" << string(50, '=') << endl;
    cout << "    COMPRESIÓN PARALELA AVANZADA DE ARCHIVOS" << endl;
    cout << "    CC3086 - Laboratorio 7" << endl;
    cout << "    Autor: DENIL JOSÉ PARADA CABRERA - 24761" << endl;
    cout << string(50, '=') << endl;
    cout << "1.  Comprimir archivo" << endl;
    cout << "2.  Descomprimir archivo" << endl;
    cout << "3.  Verificar integridad con checksum" << endl;
    cout << "4.  Benchmark avanzado con análisis estadístico" << endl;
    cout << "5.  Probar diferentes tamaños de bloque" << endl;
    cout << "6.  Configurar tamaño de bloque global" << endl;
    cout << "7.  Análisis de mecanismos de sincronización" << endl;
    cout << "8.  Stress test con archivos múltiples" << endl;
    cout << "9.  Información del sistema y capacidades" << endl;
    cout << "10. Salir" << endl;
    cout << string(50, '=') << endl;
    cout << "Selecciona una opción: ";
}

// Función para mostrar información del sistema
void showSystemInfo() {
    cout << "\n=== INFORMACIÓN DEL SISTEMA ===" << endl;
    
    // Número de CPUs
#ifdef _SC_NPROCESSORS_ONLN
    long numCPUs = sysconf(_SC_NPROCESSORS_ONLN);
    if (numCPUs > 0) {
        cout << "CPUs disponibles: " << numCPUs << endl;
    } else {
        cout << "CPUs disponibles: No disponible" << endl;
    }
#else
    cout << "CPUs disponibles: No disponible en este sistema" << endl;
#endif
    
    // Información de memoria
#if defined(_SC_PAGESIZE) && defined(_SC_PHYS_PAGES) && defined(_SC_AVPHYS_PAGES)
    long pageSize = sysconf(_SC_PAGESIZE);
    long numPages = sysconf(_SC_PHYS_PAGES);
    long availablePages = sysconf(_SC_AVPHYS_PAGES);
    
    if (pageSize > 0 && numPages > 0) {
        double totalMemGB = (pageSize * numPages) / (1024.0 * 1024.0 * 1024.0);
        cout << "Memoria total: " << fixed << setprecision(2) << totalMemGB << " GB" << endl;
        
        if (availablePages > 0) {
            double availableMemGB = (pageSize * availablePages) / (1024.0 * 1024.0 * 1024.0);
            cout << "Memoria disponible: " << fixed << setprecision(2) << availableMemGB << " GB" << endl;
        }
    } else {
        cout << "Información de memoria: No disponible" << endl;
    }
#else
    cout << "Información de memoria: No disponible en este sistema" << endl;
#endif
    
    cout << "\n=== CONFIGURACIÓN ACTUAL ===" << endl;
    cout << "Tamaño de bloque global: " << (globalBlockSize / 1024) << " KB" << endl;
    cout << "Algoritmo de compresión: DEFLATE (zlib)" << endl;
    cout << "Mecanismos de sincronización: Mutex + Condition Variables + Semáforos + Atomics" << endl;
    
    cout << "\n=== RECOMENDACIONES DE CONFIGURACIÓN ===" << endl;
#ifdef _SC_NPROCESSORS_ONLN
    long systemCPUs = sysconf(_SC_NPROCESSORS_ONLN);
    if (systemCPUs > 0) {
        cout << "• Threads recomendados: " << min(systemCPUs, 8L) << " (núcleos físicos)" << endl;
        cout << "• Threads máximos útiles: " << systemCPUs * 2 << " (con hyperthreading)" << endl;
    } else {
        cout << "• Threads recomendados: 4 (configuración por defecto)" << endl;
        cout << "• Threads máximos útiles: 8 (configuración por defecto)" << endl;
    }
#else
    cout << "• Threads recomendados: 4 (configuración por defecto)" << endl;
    cout << "• Threads máximos útiles: 8 (configuración por defecto)" << endl;
#endif
    cout << "• Tamaño de bloque óptimo: 1-2 MB para balance memoria/rendimiento" << endl;
}

// Función de stress test
void stressTest() {
    cout << "\n=== STRESS TEST ===" << endl;
    cout << "Ejecutando múltiples compresiones simultáneas..." << endl;
    
    string inputFile = "paralelismo_teoria.txt";
    ifstream test(inputFile);
    if (!test.good()) {
        cout << "Error: No se encontró el archivo " << inputFile << endl;
        return;
    }
    test.close();
    
    const int numIterations = 5;
    const int numThreads = 4;
    
    cout << "Realizando " << numIterations << " compresiones consecutivas..." << endl;
    
    vector<double> times;
    bool allSuccessful = true;
    
    auto totalStart = high_resolution_clock::now();
    
    for (int i = 0; i < numIterations; i++) {
        string outputFile = "stress_test_" + to_string(i) + ".bin";
        cout << "\nIteración " << (i + 1) << "/" << numIterations << "..." << endl;
        
        auto start = high_resolution_clock::now();
        int result = compressFile(inputFile, outputFile, numThreads);
        auto end = high_resolution_clock::now();
        
        if (result == 0) {
            auto duration = duration_cast<milliseconds>(end - start);
            times.push_back(duration.count());
            remove(outputFile.c_str()); // Limpiar
        } else {
            allSuccessful = false;
            cout << "Error en iteración " << (i + 1) << endl;
        }
    }
    
    auto totalEnd = high_resolution_clock::now();
    auto totalDuration = duration_cast<milliseconds>(totalEnd - totalStart);
    
    if (allSuccessful && !times.empty()) {
        double avgTime = 0, minTime = times[0], maxTime = times[0];
        for (double t : times) {
            avgTime += t;
            minTime = min(minTime, t);
            maxTime = max(maxTime, t);
        }
        avgTime /= times.size();
        
        double variance = 0;
        for (double t : times) {
            variance += (t - avgTime) * (t - avgTime);
        }
        variance /= times.size();
        double stdDev = sqrt(variance);
        
        cout << "\n=== RESULTADOS STRESS TEST ===" << endl;
        cout << "Tiempo total: " << totalDuration.count() << " ms" << endl;
        cout << "Tiempo promedio: " << fixed << setprecision(1) << avgTime << " ms" << endl;
        cout << "Tiempo mínimo: " << minTime << " ms" << endl;
        cout << "Tiempo máximo: " << maxTime << " ms" << endl;
        cout << "Desviación estándar: " << fixed << setprecision(1) << stdDev << " ms" << endl;
        cout << "Variabilidad: " << fixed << setprecision(1) << (stdDev / avgTime * 100) << "%" << endl;
        
        if (stdDev / avgTime < 0.1) {
            cout << "✓ Rendimiento CONSISTENTE - Sistema estable" << endl;
        } else if (stdDev / avgTime < 0.2) {
            cout << "⚠ Rendimiento MODERADO - Variabilidad aceptable" << endl;
        } else {
            cout << "✗ Rendimiento INCONSISTENTE - Revisar sistema" << endl;
        }
    } else {
        cout << "Stress test falló - verificar recursos del sistema" << endl;
    }
}

int main() {
    int option;
    string inputFile, outputFile;
    int numThreads;
    
    cout << "=== LABORATORIO 7: COMPRESIÓN PARALELA AVANZADA ===" << endl;
    cout << "Autor: DENIL JOSÉ PARADA CABRERA - 24761" << endl;
    cout << "CC3086 - Programación de Microprocesadores" << endl;
    
    // Inicializar mutex globales
    pthread_mutex_init(&writeMutex, nullptr);
    pthread_mutex_init(&printMutex, nullptr);
    pthread_mutex_init(&orderMutex, nullptr);
    pthread_cond_init(&writeCondition, nullptr);
    
    while (true) {
        showAdvancedMenu();
        cin >> option;
        
        switch (option) {
            case 1: {
                cout << "\n--- COMPRESIÓN AVANZADA ---" << endl;
                cout << "Archivo de entrada: ";
                cin >> inputFile;
                cout << "Archivo de salida: ";
                cin >> outputFile;
                cout << "Número de hilos (recomendado 4-8): ";
                cin >> numThreads;
                
                if (numThreads <= 0 || numThreads > 64) {
                    cout << "Ajustando número de hilos a rango válido (1-64)" << endl;
                    numThreads = max(1, min(64, numThreads));
                }
                
                compressFile(inputFile, outputFile, numThreads);
                break;
            }
            
            case 2: {
                cout << "\n--- DESCOMPRESIÓN AVANZADA ---" << endl;
                cout << "Archivo comprimido: ";
                cin >> inputFile;
                cout << "Archivo de salida: ";
                cin >> outputFile;
                cout << "Número de hilos: ";
                cin >> numThreads;
                
                if (numThreads <= 0 || numThreads > 64) {
                    cout << "Ajustando número de hilos a rango válido (1-64)" << endl;
                    numThreads = max(1, min(64, numThreads));
                }
                
                decompressFile(inputFile, outputFile, numThreads);
                break;
            }
            
            case 3: {
                cout << "\n--- VERIFICACIÓN DE INTEGRIDAD AVANZADA ---" << endl;
                string file1, file2;
                cout << "Archivo original: ";
                cin >> file1;
                cout << "Archivo descomprimido: ";
                cin >> file2;
                
                cout << "Verificando integridad con checksum CRC32..." << endl;
                if (verifyFiles(file1, file2)) {
                    cout << "✓ VERIFICACIÓN EXITOSA: Los archivos son idénticos" << endl;
                } else {
                    cout << "✗ VERIFICACIÓN FALLIDA: Los archivos son diferentes" << endl;
                }
                break;
            }
            
            case 4: {
                runAdvancedBenchmark();
                break;
            }
            
            case 5: {
                testBlockSizes();
                break;
            }
            
            case 6: {
                cout << "\n--- CONFIGURACIÓN DE TAMAÑO DE BLOQUE ---" << endl;
                cout << "Tamaño actual: " << (globalBlockSize / 1024) << " KB" << endl;
                cout << "Opciones:" << endl;
                cout << "1. 64 KB   2. 256 KB   3. 512 KB" << endl;
                cout << "4. 1 MB    5. 2 MB     6. 4 MB" << endl;
                cout << "Selecciona opción (1-6): ";
                
                int sizeOption;
                cin >> sizeOption;
                
                switch (sizeOption) {
                    case 1: globalBlockSize = 64 * 1024; break;
                    case 2: globalBlockSize = 256 * 1024; break;
                    case 3: globalBlockSize = 512 * 1024; break;
                    case 4: globalBlockSize = 1024 * 1024; break;
                    case 5: globalBlockSize = 2048 * 1024; break;
                    case 6: globalBlockSize = 4096 * 1024; break;
                    default: 
                        cout << "Opción inválida, manteniendo " << (globalBlockSize / 1024) << " KB" << endl;
                        break;
                }
                
                cout << "Tamaño de bloque configurado: " << (globalBlockSize / 1024) << " KB" << endl;
                break;
            }
            
            case 7: {
                analyzeSynchronization();
                break;
            }
            
            case 8: {
                stressTest();
                break;
            }
            
            case 9: {
                showSystemInfo();
                break;
            }
            
            case 10:
                cout << "\n¡Gracias por usar el programa!" << endl;
                cout << "Laboratorio completado exitosamente." << endl;
                
                // Limpiar recursos
                pthread_mutex_destroy(&writeMutex);
                pthread_mutex_destroy(&printMutex);
                pthread_mutex_destroy(&orderMutex);
                pthread_cond_destroy(&writeCondition);
                
                return 0;
                
            default:
                cout << "Opción inválida. Por favor, selecciona 1-10." << endl;
                break;
        }
        
        cout << "\nPresiona Enter para continuar...";
        cin.ignore();
        cin.get();
    }
    
    return 0;
}