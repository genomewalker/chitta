package reads
import kotlin.math.abs
interface Named
open class Reads
class Counts : Reads(), Named {
 fun total(x: Int): Int = normalize(x)
}
fun normalize(x: Int): Int = abs(x)
