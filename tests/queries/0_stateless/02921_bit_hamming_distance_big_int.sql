SELECT 314776434768051644139306697240981192872::UInt128 AS x, 0::UInt128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 14776434768051644139306697240981192872314776434768051644139306697240981192872::UInt256 AS x, 0::UInt128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 314776434768051644139306697240981192872::UInt128 AS x, 14776434768051644139306697240981192872314776434768051644139306697240981192872::UInt256 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;

SELECT 314776434768051644139306697240981192872::Int128 AS x, 0::UInt128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 14776434768051644139306697240981192872314776434768051644139306697240981192872::Int256 AS x, 0::UInt128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 314776434768051644139306697240981192872::Int128 AS x, 14776434768051644139306697240981192872314776434768051644139306697240981192872::UInt256 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;

SELECT 314776434768051644139306697240981192872::UInt128 AS x, 0::Int128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 14776434768051644139306697240981192872314776434768051644139306697240981192872::UInt256 AS x, 0::Int128 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;
SELECT 314776434768051644139306697240981192872::UInt128 AS x, 14776434768051644139306697240981192872314776434768051644139306697240981192872::Int256 AS y, bitCount(bitXor(x, y)) AS a, bitHammingDistance(x, y) AS b;

