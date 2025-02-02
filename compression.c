#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "compression.h"

char *lz77decompress(char *buffer, int size, unsigned int *uncompressedSize){
	//decompress the input buffer. 
	//input is invalid if the size is less than 4.
	if (size < 4) return NULL;

	//find the length of the decompressed buffer.
	uint32_t length = *(uint32_t *) (buffer + 1) & 0xFFFFFF;

	//create a buffer for the decompressed buffer
	char *result = (char *) malloc(length);
	if (result == NULL) return NULL;
	*uncompressedSize = length;

	//initialize variables
	uint32_t offset = 4;
	uint32_t dstOffset = 0;
	while (1) {
		uint8_t head = buffer[offset];
		offset++;
		//loop 8 times
		for (int i = 0; i < 8; i++) {
			int flag = head >> 7;
			head <<= 1;

			if (!flag) {
				result[dstOffset] = buffer[offset];
				dstOffset++, offset++;
				if(dstOffset == length) return result;
			} else {
				uint8_t high = buffer[offset++];
				uint8_t low = buffer[offset++];

				//length of uncompressed chunk and offset
				uint32_t offs = (((high & 0xF) << 8) | low) + 1;
				uint32_t len = (high >> 4) + 3;
				for (uint32_t j = 0; j < len; j++) {
					result[dstOffset] = result[dstOffset - offs];
					dstOffset++;
					if(dstOffset == length) return result;
				}
			}
		}
	}
	return result;
}

char *lz77HeaderDecompress(char *buffer, int size, int *uncompressedSize) {
	if (size < 8) return NULL;
	return lz77decompress(buffer + 4, size - 4, uncompressedSize);
}

char *lz11decompress(char *buffer, int size, int *uncompressedSize) {
	//decompress the input buffer. 
	if (size < 4) return NULL;

	//find the length of the decompressed buffer.
	uint32_t length = *(uint32_t *) (buffer) >> 8;

	//create a buffer for the decompressed buffer
	char *result = (char *) malloc(length);
	if (result == NULL) return NULL;
	*uncompressedSize = length;

	//initialize variables
	uint32_t offset = 4;
	uint32_t dstOffset = 0;
	while (1) {
		uint8_t head = buffer[offset];
		offset++;

		//loop 8 times
		for (int i = 0; i < 8; i++) {
			int flag = head >> 7;
			head <<= 1;

			if (!flag) {
				result[dstOffset] = buffer[offset];
				dstOffset++, offset++;
				if (dstOffset == length) return result;
			} else {
				uint8_t high = buffer[offset++];
				uint8_t low = buffer[offset++];
				uint8_t low2, low3;
				int mode = high >> 4;

				uint32_t len = 0, offs = 0;
				switch (mode) {
					case 0:
						low2 = buffer[offset++];
						len = ((high << 4) | (low >> 4)) + 0x11; //8-bit length +0x11
						offs = (((low & 0xF) << 8) | low2) + 1; //12-bit offset
						break;
					case 1:
						low2 = buffer[offset++];
						low3 = buffer[offset++];
						len = (((high & 0xF) << 12) | (low << 4) | (low2 >> 4)) + 0x111; //16-bit length +0x111
						offs = (((low2 & 0xF) << 8) | low3) + 1; //12-bit offset
						break;
					default:
						len = (high >> 4) + 1; //4-bit length +0x1 (but >= 3)
						offs = (((high & 0xF) << 8) | low) + 1; //12-bit offset
						break;
				}

				//write back
				for (uint32_t j = 0; j < len; j++) {
					result[dstOffset] = result[dstOffset - offs];
					dstOffset++;
					if (dstOffset == length) return result;
				}
			}
		}
	}
	return result;
}

char *huffmanDecompress(unsigned char *buffer, int size, int *uncompressedSize) {
	if (size < 5) return NULL;

	int outSize = (*(uint32_t *) buffer) >> 8;
	char *out = (char *) malloc((outSize + 3) & ~3);
	*uncompressedSize = outSize;

	unsigned char *treeBase = buffer + 4;
	int symSize = *buffer & 0xF;
	int bufferFill = 0;
	int bufferSize = 32 / symSize;
	uint32_t outBuffer = 0;

	int offs = ((*treeBase + 1) << 1) + 4;
	int trOffs = 1;

	int nWritten = 0;
	while (nWritten < outSize) {

		uint32_t bits = *(uint32_t *) (buffer + offs);
		offs += 4;

		for (int i = 0; i < 32; i++) {
			int lr = (bits >> 31) & 1;
			unsigned char thisNode = treeBase[trOffs];
			int thisNodeOffs = ((thisNode & 0x3F) + 1) << 1; //add to current offset rounded down to get next element offset

			trOffs = (trOffs & ~1) + thisNodeOffs + lr;

			if (thisNode & (0x80 >> lr)) { //reached a leaf node!
				outBuffer >>= symSize;
				outBuffer |= treeBase[trOffs] << (32 - symSize);
				trOffs = 1;
				bufferFill++;

				if (bufferFill >= bufferSize) {
					*(uint32_t *) (out + nWritten) = outBuffer;
					nWritten += 4;
					bufferFill = 0;
				}
			}
			if (nWritten >= outSize) return out;
			bits <<= 1; //next bit
		}
	}

	return out;
}

int compareMemory(char *b1, char *b2, int nMax, int nAbsoluteMax) {
	int nSame = 0;
	if (nMax > nAbsoluteMax) nMax = nAbsoluteMax;
	//count up to nMax. If all match, count 0x12-nMax bytes. The b1 just starts over.
	for (int i = 0; i < nMax; i++) {
		if (*(b1++) == *(b2++)) nSame++;
		else break;
	}
	if (nSame == nMax) {
		b1 -= nMax;
		for (int i = 0; i < nAbsoluteMax - nMax; i++) {
			if (*(b1++) == *(b2++)) nSame++;
			else break;
		}
	}
	return nSame;
}

char *lz77compress(char *buffer, int size, unsigned int *compressedSize){
	int compressedMaxSize = 4 + 9 * ((size + 7) >> 3);
	char *compressed = (char *) malloc(compressedMaxSize);
	char *compressedBase = compressed;
	*(unsigned *) compressed = size << 8;
	*compressed = 0x10;
	int nProcessedBytes = 0;
	int nSize = 4;
	compressed += 4;
	while (1) {
		//make note of where to store the head for later.
		char *headLocation = compressed;
		compressed++;
		nSize++;
		//initialize the head.
		char head = 0;

		//set when the end of the file is reached, and the result needs to be zero-padded.
		int isDone = 0;

		//repeat 8x (8 bits per byte)
		for (int i = 0; i < 8; i++) {
			head <<= 1;

			if (isDone) {
				*(compressed++) = 0;
				nSize++;
				continue;
			}

			//search backwards up to 0xFFF bytes.
			int maxSearch = 0x1000;
			if (maxSearch > nProcessedBytes) maxSearch = nProcessedBytes;

			//the biggest match, and where it was
			int biggestRun = 0, biggestRunIndex = 0;

			//begin searching backwards.
			for (int j = 2; j < maxSearch; j++) {
				//compare up to 0xF bytes, at most j bytes.
				int nCompare = 0x12;
				if (nCompare > j) nCompare = j;
				int nBytesLeft = size - nProcessedBytes;
				int nAbsoluteMaxCompare = 0x12;
				if (nAbsoluteMaxCompare > nBytesLeft) nAbsoluteMaxCompare = nBytesLeft;
				int nMatched = compareMemory(buffer - j, buffer, nCompare, nAbsoluteMaxCompare);
				if (nMatched > biggestRun) {
					if (biggestRun == 0x12) break;
					biggestRun = nMatched;
					biggestRunIndex = j;
				}
			}

			//if the biggest run is at least 3, then we use it.
			if (biggestRun >= 3) {
				head |= 1;
				nProcessedBytes += biggestRun;
				//encode the match. First byte has high nybble as length minus 3, low nybble as the high byte of the offset.
				*(compressed++) = ((biggestRun - 3) << 4) | (((biggestRunIndex - 1) >> 8) & 0xF);
				*(compressed++) = (biggestRunIndex - 1) & 0xFF;
				//advance the buffer
				buffer += biggestRun;
				nSize += 2;
				if (nProcessedBytes >= size) isDone = 1;
			} else {
				*(compressed++) = *(buffer++);
				nProcessedBytes++;
				nSize++;
				if (nProcessedBytes >= size) isDone = 1;
			}
		}
		*headLocation = head;
		if (nProcessedBytes >= size) break;
	}
	*compressedSize = nSize;
	return realloc(compressedBase, nSize);

}//22999

char *lz77HeaderCompress(char *buffer, int size, int *compressedSize) {
	char *compressed = lz77compress(buffer, size, compressedSize);
	if (compressed == NULL) return NULL;
	*compressedSize += 4;
	compressed = realloc(compressed, *compressedSize);
	memmove(compressed + 4, compressed, *compressedSize - 4);
	compressed[0] = 'L';
	compressed[1] = 'Z';
	compressed[2] = '7';
	compressed[3] = '7';
	return compressed;
}

char *lz11compress(char *buffer, int size, int *compressedSize) {
	int compressedMaxSize = 7 + 9 * ((size + 7) >> 3);
	char *compressed = (char *) malloc(compressedMaxSize);
	char *compressedBase = compressed;
	*(unsigned *) compressed = size << 8;
	*compressed = 0x11;
	int nProcessedBytes = 0;
	int nSize = 4;
	compressed += 4;
	while (1) {
		//make note of where to store the head for later.
		char *headLocation = compressed;
		compressed++;
		nSize++;
		//initialize the head.
		char head = 0;

		//set when the end of the file is reached, and the result needs to be zero-padded.
		int isDone = 0;

		//repeat 8x (8 bits per byte)
		for (int i = 0; i < 8; i++) {
			head <<= 1;

			if (isDone) {
				*(compressed++) = 0;
				nSize++;
				continue;
			}

			//search backwards up to 0xFFF bytes.
			int maxSearch = 0x1000;
			if (maxSearch > nProcessedBytes) maxSearch = nProcessedBytes;

			//the biggest match, and where it was
			int biggestRun = 0, biggestRunIndex = 0;

			//begin searching backwards.
			for (int j = 2; j < maxSearch; j++) {
				//compare up to 0xF bytes, at most j bytes.
				int nCompare = 0xFFFF + 0x111; //max run length
				if (nCompare > j) nCompare = j;
				int nBytesLeft = size - nProcessedBytes;
				int nAbsoluteMaxCompare = 0xFFFF + 0x111; //max run length
				if (nAbsoluteMaxCompare > nBytesLeft) nAbsoluteMaxCompare = nBytesLeft;
				int nMatched = compareMemory(buffer - j, buffer, nCompare, nAbsoluteMaxCompare);
				if (nMatched > biggestRun) {
					if (biggestRun == 0x12) break;
					biggestRun = nMatched;
					biggestRunIndex = j;
				}
			}

			//if the biggest run is at least 3, then we use it.
			if (biggestRun >= 3) {
				head |= 1;
				nProcessedBytes += biggestRun;
				//encode the match.

				if (biggestRun <= 0x10) {
					//First byte has high nybble as length minus 1, low nybble as the high byte of the offset.
					*(compressed++) = ((biggestRun - 1) << 4) | (((biggestRunIndex - 1) >> 8) & 0xF);
					*(compressed++) = (biggestRunIndex - 1) & 0xFF;
					nSize += 2;
				} else if (biggestRun <= 0xFF + 0x11) {
					//First byte has the high 4 bits of run length minus 0x11
					//Second byte has the low 4 bits of the run length minus 0x11 in the high nybble
					*(compressed++) = (biggestRun - 0x11) >> 4;
					*(compressed++) = (((biggestRun - 0x11) & 0xF) << 4) | ((biggestRunIndex - 1) >> 8);
					*(compressed++) = (biggestRunIndex - 1) & 0xFF;
					nSize += 3;
				} else if (biggestRun <= 0xFFFF + 0x111) {
					//First byte is 0x10 ORed with the high 4 bits of run length minus 0x111
					*(compressed++) = 0x10 | (((biggestRun - 0x111) >> 12) & 0xF);
					*(compressed++) = ((biggestRun - 0x111) >> 4) & 0xFF;
					*(compressed++) = (((biggestRun - 0x111) & 0xF) << 4) | (((biggestRunIndex - 1) >> 8) & 0xF);
					*(compressed++) = (biggestRunIndex - 1) & 0xFF;

					nSize += 4;
				}
				//advance the buffer
				buffer += biggestRun;
				if (nProcessedBytes >= size) isDone = 1;
			} else {
				*(compressed++) = *(buffer++);
				nProcessedBytes++;
				nSize++;
				if (nProcessedBytes >= size) isDone = 1;
			}
		}
		*headLocation = head;
		if (nProcessedBytes >= size) break;
	}

	while (nSize & 3) {
		*(compressed++) = 0;
		nSize++;
	}
	*compressedSize = nSize;
	return realloc(compressedBase, nSize);
}

typedef struct HUFFNODE_ {
	unsigned char sym;
	unsigned char symMin; //had space to spare, maybe make searches a little simpler
	unsigned char symMax;
	unsigned char nRepresent;
	int freq;
	struct HUFFNODE_ *left;
	struct HUFFNODE_ *right;
} HUFFNODE;

typedef struct BITSTREAM_ {
	unsigned *bits;
	int nWords;
	int nBitsInLastWord;
	int nWordsAlloc;
} BITSTREAM;

void bitStreamCreate(BITSTREAM *stream) {
	stream->nWords = 1;
	stream->nBitsInLastWord = 0;
	stream->nWordsAlloc = 16;
	stream->bits = (unsigned *) calloc(16, 4);
}

void bitStreamFree(BITSTREAM *stream) {
	free(stream->bits);
}

void bitStreamWrite(BITSTREAM *stream, int bit) {
	if (stream->nBitsInLastWord == 32) {
		stream->nBitsInLastWord = 0;
		stream->nWords++;
		if (stream->nWords > stream->nWordsAlloc) {
			int newAllocSize = (stream->nWordsAlloc + 2) * 3 / 2;
			stream->bits = realloc(stream->bits, newAllocSize * 4);
			stream->nWordsAlloc = newAllocSize;
		}
		stream->bits[stream->nWords - 1] = 0;
	}

	stream->bits[stream->nWords - 1] |= (bit << (31 - stream->nBitsInLastWord));
	stream->nBitsInLastWord++;
}

#define ISLEAF(n) ((n)->left==NULL&&(n)->right==NULL)

int huffNodeComparator(const void *p1, const void *p2) {
	return ((HUFFNODE *) p2)->freq - ((HUFFNODE *) p1)->freq;
}

unsigned int huffmanWriteNode(unsigned char *tree, unsigned int pos, HUFFNODE *node) {
	HUFFNODE *left = node->left;
	HUFFNODE *right = node->right;

	//we will write two bytes. 
	unsigned int afterPos = pos + 2;
	if (ISLEAF(left)) {
		tree[pos] = left->sym;
	} else {
		HUFFNODE *leftLeft = left->left;
		HUFFNODE *leftRight = left->right;
		unsigned char flag = (ISLEAF(leftLeft) << 7) | (ISLEAF(leftRight) << 6);
		unsigned lastAfterPos = afterPos;
		afterPos = huffmanWriteNode(tree, afterPos, left);
		tree[pos] = flag | ((((lastAfterPos - pos) >> 1) - 1) & 0x3F);
		if (((lastAfterPos - pos) >> 1) - 1 > 0x3F) _asm int 3;
	}

	if (ISLEAF(right)) {
		tree[pos + 1] = right->sym;
	} else {
		HUFFNODE *rightLeft = right->left;
		HUFFNODE *rightRight = right->right;
		unsigned char flag = (ISLEAF(rightLeft) << 7) | (ISLEAF(rightRight) << 6);
		unsigned lastAfterPos = afterPos;
		afterPos = huffmanWriteNode(tree, afterPos, right);
		tree[pos + 1] = flag | ((((lastAfterPos - pos) >> 1) - 1) & 0x3F);
		if (((lastAfterPos - pos) >> 1) - 1 > 0x3F) _asm int 3;
	}
	return afterPos;
}

void makeShallowNodeFirst(HUFFNODE *node) {
	if (ISLEAF(node)) return;
	if (node->left->nRepresent > node->right->nRepresent) {
		HUFFNODE *left = node->left;
		node->left = node->right;
		node->right = left;
	}
	makeShallowNodeFirst(node->left);
	makeShallowNodeFirst(node->right);
}

int huffmanNodeHasSymbol(HUFFNODE *node, unsigned char sym) {
	if (ISLEAF(node)) return node->sym == sym;
	if (sym < node->symMin || sym > node->symMax) return 0;
	HUFFNODE *left = node->left;
	HUFFNODE *right = node->right;
	return huffmanNodeHasSymbol(left, sym) || huffmanNodeHasSymbol(right, sym);
}

void huffmanWriteSymbol(BITSTREAM *bits, unsigned char sym, HUFFNODE *tree) {
	if (ISLEAF(tree)) return;
	HUFFNODE *left = tree->left;
	HUFFNODE *right = tree->right;
	if (huffmanNodeHasSymbol(left, sym)) {
		bitStreamWrite(bits, 0);
		huffmanWriteSymbol(bits, sym, left);
	} else {
		bitStreamWrite(bits, 1);
		huffmanWriteSymbol(bits, sym, right);
	}
}

void huffmanConstructTree(HUFFNODE *nodes, int nNodes) {
	//sort by frequency, then cut off the remainder (freq=0).
	qsort(nodes, nNodes, sizeof(HUFFNODE), huffNodeComparator);
	for (int i = 0; i < nNodes; i++) {
		if (nodes[i].freq == 0) {
			nNodes = i;
			break;
		}
	}

	//unflatten the histogram into a huffman tree. 
	int nRoots = nNodes;
	int nTotalNodes = nNodes;
	while (nRoots > 1) {
		//copy bottom two nodes to just outside the current range
		HUFFNODE *srcA = nodes + nRoots - 2;
		HUFFNODE *destA = nodes + nTotalNodes;
		memcpy(destA, srcA, sizeof(HUFFNODE));

		HUFFNODE *left = destA;
		HUFFNODE *right = nodes + nRoots - 1;
		HUFFNODE *branch = srcA;

		branch->freq = left->freq + right->freq;
		branch->sym = 0;
		branch->left = left;
		branch->right = right;
		branch->symMin = min(left->symMin, right->symMin);
		branch->symMax = max(right->symMax, left->symMax);
		branch->nRepresent = left->nRepresent + right->nRepresent; //may overflow for root, but the root doesn't really matter for this

		nRoots--;
		nTotalNodes++;
		qsort(nodes, nRoots, sizeof(HUFFNODE), huffNodeComparator);
	}

	//just to be sure, make sure the shallow node always comes first
	makeShallowNodeFirst(nodes);
}

char *huffmanCompress(unsigned char *buffer, int size, int *compressedSize, int nBits) {
	if (nBits == 8) nBits = 4; //HACK: Force 4-bit Huffman Compression until 8-bit Huffman Compression is fixed
	//create a histogram of each byte in the file.
	HUFFNODE *nodes = (HUFFNODE *) calloc(512, sizeof(HUFFNODE));
	int nSym = 1 << nBits;
	for (int i = 0; i < nSym; i++) {
		nodes[i].sym = i;
		nodes[i].symMin = i;
		nodes[i].symMax = i;
		nodes[i].nRepresent = 1;
	}

	//construct histogram
	if (nBits == 8) {
		for (int i = 0; i < size; i++) {
			nodes[buffer[i]].freq++;
		}
	} else {
		for (int i = 0; i < size; i++) {
			nodes[buffer[i] & 0xF].freq++;
			nodes[buffer[i] >> 4].freq++;
		}
	}

	huffmanConstructTree(nodes, nSym);

	//now we've got a proper Huffman tree. Great! 
	unsigned char *tree = (unsigned char *) calloc(512, 1);
	uint32_t treeSize = huffmanWriteNode(tree, 2, nodes);
	treeSize = (treeSize + 3) & ~3; //round up
	tree[0] = (treeSize >> 1) - 1;
	tree[1] = 0;

	//now write bits out.
	BITSTREAM stream;
	bitStreamCreate(&stream);
	if (nBits == 8) {
		for (int i = 0; i < size; i++) {
			huffmanWriteSymbol(&stream, buffer[i], nodes);
		}
	} else {
		for (int i = 0; i < size; i++) {
			huffmanWriteSymbol(&stream, buffer[i] & 0xF, nodes);
			huffmanWriteSymbol(&stream, buffer[i] >> 4, nodes);
		}
	}

	//combine into one
	uint32_t outSize = 4 + treeSize + stream.nWords * 4;
	char *finBuf = (char *) malloc(outSize);
	*(uint32_t *) finBuf = 0x20 | nBits | (size << 8);
	memcpy(finBuf + 4, tree, treeSize);
	memcpy(finBuf + 4 + treeSize, stream.bits, stream.nWords * 4);
	free(tree);
	free(nodes);
	bitStreamFree(&stream);

	*compressedSize = outSize;
	return finBuf;
}

char *huffman8Compress(unsigned char *buffer, int size, int *compressedSize) {
	return huffmanCompress(buffer, size, compressedSize, 8);
}

char *huffman4Compress(unsigned char *buffer, int size, int *compressedSize) {
	return huffmanCompress(buffer, size, compressedSize, 4);
}

int lz77IsCompressed(char *buffer, unsigned int size) {
	if (size < 4) return 0;
	if (*buffer != 0x10) return 0;
	uint32_t length = (*(uint32_t *) buffer) >> 8;
	if ((length / 144) * 17 + 4 > size) return 0;

	//start a dummy decompression
	uint32_t offset = 4;
	uint32_t dstOffset = 0;
	while (1) {
		uint8_t head = buffer[offset];
		offset++;

		//loop 8 times
		for(int i = 0; i < 8; i++){
			int flag = head >> 7;
			head <<= 1;

			if (!flag) {
				if (dstOffset >= length || offset >= size) return 0;
				dstOffset++, offset++;
				if (dstOffset == length) return 1;
			} else {
				if (offset + 1 >= size) return 0;
				uint8_t high = buffer[offset++];
				uint8_t low = buffer[offset++];

				//length of uncompressed chunk and offset
				uint32_t offs = (((high & 0xF) << 8) | low) + 1;
				uint32_t len = (high >> 4) + 3;

				if (dstOffset < offs) return 0;
				for (uint32_t j = 0; j < len; j++) {
					if (dstOffset >= length) return 0;
					dstOffset++;
					if (dstOffset == length) return 1;
				}
			}
		}
	}
	return 1;
}

int lz77HeaderIsCompressed(unsigned char *buffer, unsigned size) {
	if (size < 8) return 0;
	if (buffer[0] != 'L' || buffer[1] != 'Z' || buffer[2] != '7' || buffer[3] != '7') return 0;
	return lz77IsCompressed(buffer + 4, size - 4);
}

int lz11IsCompressed(char *buffer, unsigned size) {
	if (size < 4) return 0;
	if (*buffer != 0x11) return 0;

	uint32_t length = (*(uint32_t *) buffer) >> 8;
	if (size > 7 + length * 9 / 8) return 0;

	//perform a test decompression.
	uint32_t offset = 4;
	uint32_t dstOffset = 0;
	while (1) {
		if (offset >= size) return 0;
		uint8_t head = buffer[offset];
		uint8_t origHead = head;
		offset++;

		//loop 8 times
		for (int i = 0; i < 8; i++) {
			int flag = head >> 7;
			head <<= 1;

			if (!flag) {
				if (offset >= size || dstOffset >= length) return 0;
				dstOffset++, offset++;
				if (dstOffset == length) return 1;
			} else {
				if (offset + 1 >= size) return 0;
				uint8_t high = buffer[offset++];
				uint8_t low = buffer[offset++];
				uint8_t low2, low3;
				int mode = high >> 4;

				uint32_t len = 0, offs = 0;
				switch (mode) {
					case 0:
						if (offset >= size) return 0;
						low2 = buffer[offset++];
						len = ((high << 4) | (low >> 4)) + 0x11; //8-bit length +0x11
						offs = (((low & 0xF) << 8) | low2) + 1; //12-bit offset
						break;
					case 1:
						if (offset + 1 >= size) return 0;
						low2 = buffer[offset++];
						low3 = buffer[offset++];
						len = (((high & 0xF) << 12) | (low << 4) | (low2 >> 4)) + 0x111; //16-bit length +0x111
						offs = (((low2 & 0xF) << 8) | low3) + 1; //12-bit offset
						break;
					default:
						len = (high >> 4) + 1; //4-bit length +0x1 (but >= 3)
						offs = (((high & 0xF) << 8) | low) + 1; //12-bit offset
						break;
				}

				//test write
				if (dstOffset < offs) return 0; //would we write before our buffer decompressing? (weird because unsigned)
				for (uint32_t j = 0; j < len; j++) {
					if (dstOffset >= length) return 0;
					dstOffset++;
					if (dstOffset == length) return 1;
				}
			}
		}
	}

	return 1;
}

int huffmanIsCompressed(unsigned char *buffer, unsigned size) {
	if (size < 5) return 0;
	if (*buffer != 0x24 && *buffer != 0x28) return 0;

	uint32_t length = (*(uint32_t *) buffer) >> 8;
	uint32_t bitStreamOffset = ((buffer[5] + 1) << 1) + 4;
	if (bitStreamOffset > size) return 0;

	//process huffman tree
	uint32_t dataOffset = ((buffer[4] + 1) << 1) + 4;
	if (dataOffset > size) return 0;

	//Do Test Decompression
	unsigned char *treeBase = buffer + 4;
	int symSize = *buffer & 0xF;
	int bufferFill = 0;
	int bufferSize = 32 / (*buffer - 0x20);
	uint32_t outBuffer = 0;

	int trOffs = 1;

	int nWritten = 0;
	while (nWritten < length) {

		uint32_t bits = *(uint32_t *)(buffer + dataOffset);
		dataOffset += 4;

		for (int i = 0; i < 32; i++) {
			int lr = (bits >> 31) & 1;
			unsigned char thisNode = treeBase[trOffs];
			int thisNodeOffs = ((thisNode & 0x3F) + 1) << 1; //add to current offset rounded down to get next element offset

			trOffs = (trOffs & ~1) + thisNodeOffs + lr;

			if (thisNode & (0x80 >> lr)) { //reached a leaf node!
				outBuffer >>= symSize;
				outBuffer |= treeBase[trOffs] << (32 - symSize);
				trOffs = 1;
				bufferFill++;

				if (bufferFill >= bufferSize) {
					nWritten += 4;
					bufferFill = 0;
				}
			}
			if (nWritten >= length) {
				return size == dataOffset;
			}
			bits <<= 1; //next bit
		}
	}
	return size == dataOffset;
}

int huffman4IsCompressed(unsigned char *buffer, unsigned size) {
	return size > 0 && *buffer == 0x24 && huffmanIsCompressed(buffer, size);
}

int huffman8IsCompressed(unsigned char *buffer, unsigned size) {
	return size > 0 && *buffer == 0x28 && huffmanIsCompressed(buffer, size);
}

int getCompressionType(char *buffer, int size) {
	if (lz77HeaderIsCompressed(buffer, size)) return COMPRESSION_LZ77_HEADER;
	if (lz77IsCompressed(buffer, size)) return COMPRESSION_LZ77;
	if (lz11IsCompressed(buffer, size)) return COMPRESSION_LZ11;
	if (huffman4IsCompressed(buffer, size)) return COMPRESSION_HUFFMAN_4;
	if (huffman8IsCompressed(buffer, size)) return COMPRESSION_HUFFMAN_8;

	return COMPRESSION_NONE;
}

char *decompress(char *buffer, int size, int *uncompressedSize) {
	int type = getCompressionType(buffer, size);
	switch (type) {
		case COMPRESSION_NONE:
		{
			void *copy = malloc(size);
			memcpy(copy, buffer, size);
			*uncompressedSize = size;
			return copy;
		}
		case COMPRESSION_LZ77:
			return lz77decompress(buffer, size, uncompressedSize);
		case COMPRESSION_LZ11:
			return lz11decompress(buffer, size, uncompressedSize);
		case COMPRESSION_HUFFMAN_4:
		case COMPRESSION_HUFFMAN_8:
			return huffmanDecompress(buffer, size, uncompressedSize);
		case COMPRESSION_LZ77_HEADER:
			return lz77HeaderDecompress(buffer, size, uncompressedSize);
	}
	return NULL;
}

char *compress(char *buffer, int size, int compression, int *compressedSize) {
	switch (compression) {
		case COMPRESSION_NONE:
		{
			void *copy = malloc(size);
			memcpy(copy, buffer, size);
			*compressedSize = size;
			return copy;
		}
		case COMPRESSION_LZ77:
			return lz77compress(buffer, size, compressedSize);
		case COMPRESSION_LZ11:
			return lz11compress(buffer, size, compressedSize);
		case COMPRESSION_HUFFMAN_4:
			return huffman4Compress(buffer, size, compressedSize); //Commented out because 
		case COMPRESSION_HUFFMAN_8:
			return huffman8Compress(buffer, size, compressedSize);
		case COMPRESSION_LZ77_HEADER:
			return lz77HeaderCompress(buffer, size, compressedSize);
	}
	return NULL;
}

const char *getCompressionTypeName(int type)
{
	switch(type) {
		case COMPRESSION_LZ77:
			return "COMPRESSION_LZ77";
			
		case COMPRESSION_LZ11:
			return "COMPRESSION_LZ11";
			
		case COMPRESSION_HUFFMAN_4:
			return "COMPRESSION_HUFFMAN_4";
			
		case COMPRESSION_HUFFMAN_8:
			return "COMPRESSION_HUFFMAN_8";
			
		case COMPRESSION_LZ77_HEADER:
			return "COMPRESSION_LZ77_HEADER";
		
		default:
			return "COMPRESSION_NONE";
	}
}

int getCompressionTypeId(const char *name)
{
	if(!strcmp("COMPRESSION_LZ77", name)) {
		return COMPRESSION_LZ77;
	} else if (!strcmp("COMPRESSION_LZ11", name)) {
		return COMPRESSION_LZ11;
	} else if (!strcmp("COMPRESSION_HUFFMAN_4", name)) {
		return COMPRESSION_HUFFMAN_4;
	} else if (!strcmp("COMPRESSION_HUFFMAN_8", name)) {
		return COMPRESSION_HUFFMAN_8;
	} else if (!strcmp("COMPRESSION_LZ77_HEADER", name)) {
		return COMPRESSION_LZ77_HEADER;
	} else {
		return COMPRESSION_NONE;
	}
}